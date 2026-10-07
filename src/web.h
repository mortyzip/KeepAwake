/* State and tiny HTTP server for the KeepAwake control page, shared by the
   PS5 and PS4 builds.

   Keep Awake can be on (resetting the idle timer) or off (paused, payload
   still running so it can be turned back on from the page). When on, it
   runs in one of three modes:
     always    with no time limit
     timer     for a set time, then it turns itself off
     auto      only while the network is busy (a download, FTP transfer,
               PKG install...), plus a quiet period after it stops
   Quitting ends the payload.

   Routes:
     GET  /                  the control page (src/index.html)
     GET  /status            JSON status
     POST /on                keep the console awake with no time limit
     POST /on?minutes=N      keep it awake for N minutes, then turn off
     POST /on?auto=1         keep it awake only while transferring
     POST /off               stop keeping it awake, but stay running
     POST /quit              end the payload
     POST /settings?open_on_start=0|1&auto_threshold_kb=N&auto_quiet_minutes=N
                             change any of the settings (saved to
                             KEEPAWAKE_SETTINGS)

   The platform source defines WEB_CONSOLE ("PS5" or "PS4") before including
   this file, and implements the functions declared below. Client sockets are
   expected to be non-blocking. */

#include "index_html.h"


#define WEB_AGAIN       -1
#define WEB_ERROR       -2

#define WEB_TIMEOUT_MS  2000
#define WEB_POLL_MS     10


/* Bytes read, 0 if the client closed, WEB_AGAIN if no data yet, or WEB_ERROR. */
static int web_recv(int fd, void *buf, size_t len);

/* Bytes sent, WEB_AGAIN if the socket is full, or WEB_ERROR. */
static int web_send(int fd, const void *buf, size_t len);

static void web_sleep_ms(int ms);

/* Seconds and milliseconds since the payload started. */
static long long web_uptime(void);
static long long web_uptime_ms(void);

/* The console's LAN IP address as text, or an empty string if unknown. */
static void web_get_ip(char *buf, size_t size);

/* Total bytes received and sent on all non-loopback interfaces.
   Returns 0, or -1 if the counters can't be read. */
static int web_net_bytes(unsigned long long *rx, unsigned long long *tx);

/* Show a toast on the console. */
static void web_notify(const char *msg);


#define KA_MAX_MINUTES  (7 * 24 * 60)

#ifndef KEEPAWAKE_SETTINGS
#define KEEPAWAKE_SETTINGS "/data/keepawake.cfg"
#endif


static int       ka_active = 1;     // resetting the idle timer?
static long long ka_changed = 0;    // web_uptime() when on/off last changed
static long long ka_next_tick = 0;  // web_uptime() when the next tick is due
static long long ka_until = 0;      // web_uptime() when the timer ends, 0 if none
static long long ka_minutes = 0;    // length of the current timer
static int       ka_auto = 0;       // on only while transferring?

// Settings, saved to KEEPAWAKE_SETTINGS.
static int       ka_open_on_start = 1;     // open the page in the console's browser at start
static long long ka_threshold_kb = 500;    // speed that counts as transferring, KB/s
static long long ka_quiet_minutes = 5;     // stay awake this long after a transfer stops

#define KA_MIN_THRESHOLD_KB   10
#define KA_MAX_THRESHOLD_KB   (100 * 1024)
#define KA_MIN_QUIET_MINUTES  1
#define KA_MAX_QUIET_MINUTES  120


/* --- Settings ------------------------------------------------------------ */

/* Find "key=<number>" in the settings text. Returns 1 if found. */
static int
ka_cfg_number(const char *buf, const char *key, long long *value) {
  const char *p = buf;
  size_t k = strlen(key);
  long long n = 0;
  int digits = 0;

  while((p = strstr((char*)p, (char*)key))) {
    if((p == buf || p[-1] == '\n') && p[k] == '=') {
      for(p += k + 1; *p >= '0' && *p <= '9' && digits < 9; p++, digits++) {
        n = n * 10 + (*p - '0');
      }
      if(digits) {
        *value = n;
        return 1;
      }
      return 0;
    }
    p += k;
  }
  return 0;
}


static void
ka_load_settings(void) {
  char buf[256];
  long long v;
  int fd;
  int n;

  if((fd = open(KEEPAWAKE_SETTINGS, O_RDONLY, 0)) < 0) {
    return;   // no file yet: keep the defaults
  }
  n = read(fd, buf, sizeof buf - 1);
  close(fd);
  if(n <= 0) {
    return;
  }
  buf[n] = 0;

  if(ka_cfg_number(buf, "open_on_start", &v) && v <= 1) {
    ka_open_on_start = (int)v;
  }
  if(ka_cfg_number(buf, "auto_threshold_kb", &v) &&
     v >= KA_MIN_THRESHOLD_KB && v <= KA_MAX_THRESHOLD_KB) {
    ka_threshold_kb = v;
  }
  if(ka_cfg_number(buf, "auto_quiet_minutes", &v) &&
     v >= KA_MIN_QUIET_MINUTES && v <= KA_MAX_QUIET_MINUTES) {
    ka_quiet_minutes = v;
  }
}


static int
ka_save_settings(void) {
  char buf[160];
  int fd;
  int n;
  int ok;

  n = snprintf(buf, sizeof buf,
               "open_on_start=%d\n"
               "auto_threshold_kb=%lld\n"
               "auto_quiet_minutes=%lld\n",
               ka_open_on_start, ka_threshold_kb, ka_quiet_minutes);
  if((fd = open(KEEPAWAKE_SETTINGS, O_WRONLY | O_CREAT | O_TRUNC, 0644)) < 0) {
    return -1;
  }
  ok = write(fd, buf, n) == n;
  close(fd);

  return ok ? 0 : -1;
}


/* --- Transfer detection --------------------------------------------------

   Every KA_SAMPLE_MS the interface byte counters are sampled. The speed is
   the change across the last KA_SAMPLES samples (about 10 seconds), which
   smooths out bursty traffic. The network counts as busy while either
   direction is at or above the threshold. */

#define KA_SAMPLES     6
#define KA_SAMPLE_MS   2000

static struct {
  long long ms;
  unsigned long long rx;
  unsigned long long tx;
} ka_samples[KA_SAMPLES];

static int       ka_nsamples = 0;
static int       ka_sample_pos = 0;      // where the next sample goes
static long long ka_next_sample_ms = 0;
static int       ka_net_ok = 0;          // counters readable?
static long long ka_rx_rate = 0;         // bytes per second
static long long ka_tx_rate = 0;
static long long ka_last_busy = -1;      // web_uptime() when last busy, -1 if never
static int       ka_auto_awake = 0;      // auto mode currently keeping it awake?


static int
ka_transferring(void) {
  long long rate = ka_rx_rate > ka_tx_rate ? ka_rx_rate : ka_tx_rate;
  return ka_net_ok && rate >= ka_threshold_kb * 1024;
}


static void
ka_sample(void) {
  unsigned long long rx;
  unsigned long long tx;
  long long ms = web_uptime_ms();
  int newest;
  int oldest;

  if(ms < ka_next_sample_ms) {
    return;
  }
  ka_next_sample_ms = ms + KA_SAMPLE_MS;

  if(web_net_bytes(&rx, &tx) != 0) {
    ka_net_ok = 0;
    ka_rx_rate = ka_tx_rate = 0;
    return;
  }
  ka_net_ok = 1;

  // Counters that go backwards were reset (or wrapped): start over.
  if(ka_nsamples) {
    newest = (ka_sample_pos + KA_SAMPLES - 1) % KA_SAMPLES;
    if(rx < ka_samples[newest].rx || tx < ka_samples[newest].tx) {
      ka_nsamples = 0;
    }
  }

  ka_samples[ka_sample_pos].ms = ms;
  ka_samples[ka_sample_pos].rx = rx;
  ka_samples[ka_sample_pos].tx = tx;
  ka_sample_pos = (ka_sample_pos + 1) % KA_SAMPLES;
  if(ka_nsamples < KA_SAMPLES) {
    ka_nsamples++;
  }

  newest = (ka_sample_pos + KA_SAMPLES - 1) % KA_SAMPLES;
  oldest = (ka_sample_pos + KA_SAMPLES - ka_nsamples) % KA_SAMPLES;
  if(ka_nsamples >= 2 && ka_samples[newest].ms > ka_samples[oldest].ms) {
    long long span = ka_samples[newest].ms - ka_samples[oldest].ms;
    ka_rx_rate = (long long)(ka_samples[newest].rx - ka_samples[oldest].rx) * 1000 / span;
    ka_tx_rate = (long long)(ka_samples[newest].tx - ka_samples[oldest].tx) * 1000 / span;
  } else {
    ka_rx_rate = ka_tx_rate = 0;
  }

  if(ka_transferring()) {
    ka_last_busy = web_uptime();
  }
}


/* Seconds auto mode will keep the console awake for if nothing else is
   transferred: the quiet period counting down since the last busy sample. */
static long long
ka_quiet_left(void) {
  long long left;

  if(ka_last_busy < 0) {
    return 0;
  }
  left = ka_last_busy + ka_quiet_minutes * 60 - web_uptime();
  return left > 0 ? left : 0;
}


/* --- On and off ---------------------------------------------------------- */

/* "1 hour 30 minutes", "2 hours", "45 minutes" */
static void
ka_format_minutes(char *buf, size_t size, long long minutes) {
  long long h = minutes / 60;
  long long m = minutes % 60;

  if(h && m) {
    snprintf(buf, size, "%lld hour%s %lld minute%s",
             h, h == 1 ? "" : "s", m, m == 1 ? "" : "s");
  } else if(h) {
    snprintf(buf, size, "%lld hour%s", h, h == 1 ? "" : "s");
  } else {
    snprintf(buf, size, "%lld minute%s", m, m == 1 ? "" : "s");
  }
}


static void
ka_set_on(void) {
  if(!ka_active) {
    ka_active = 1;
    ka_changed = web_uptime();
    ka_next_tick = 0;
  }
}


/* Turn on, for the given minutes or with no time limit (0). Also used to
   change or remove the timer while already on. */
static void
ka_turn_on(long long minutes) {
  int was_plain_on = ka_active && !ka_until && !ka_auto;
  char duration[64];
  char msg[128];

  ka_set_on();
  ka_auto = 0;
  ka_auto_awake = 0;
  ka_until = minutes ? web_uptime() + minutes * 60 : 0;
  ka_minutes = minutes;

  if(minutes) {
    ka_format_minutes(duration, sizeof duration, minutes);
    snprintf(msg, sizeof msg, "Keep Awake on for %s", duration);
    web_notify(msg);
  } else if(!was_plain_on) {
    web_notify("Keep Awake turned on\nNo time limit");
  }
}


/* Turn on in auto mode: awake only while transferring. */
static void
ka_turn_on_auto(void) {
  if(ka_active && ka_auto) {
    return;
  }

  ka_set_on();
  ka_auto = 1;
  ka_until = 0;
  ka_minutes = 0;
  ka_auto_awake = ka_quiet_left() > 0;
  ka_next_tick = 0;

  web_notify(ka_auto_awake ?
             "Keep Awake on while transferring\nTransfer in progress" :
             "Keep Awake on while transferring\nWaiting for a transfer");
}


static void
ka_turn_off(const char *msg) {
  if(!ka_active) {
    return;
  }

  ka_active = 0;
  ka_until = 0;
  ka_auto = 0;
  ka_auto_awake = 0;
  ka_changed = web_uptime();
  web_notify(msg);
}


/* --- HTTP ---------------------------------------------------------------- */

/* Does the request line start with "<method> <path>", followed by a query
   string or the end of the path? */
static int
web_route(const char *req, size_t len, const char *method, const char *path) {
  size_t m = strlen(method);
  size_t p = strlen(path);
  char next;

  if(len < m + 1 + p + 1 || memcmp(req, method, m) != 0 || req[m] != ' ' ||
     memcmp(req + m + 1, path, p) != 0) {
    return 0;
  }
  next = req[m + 1 + p];
  return next == ' ' || next == '?';
}


/* Read a whole-number query parameter from the request line.
   Returns 1 and sets *value if present and valid, 0 if absent, -1 if invalid. */
static int
web_query_number(const char *req, size_t len, const char *key, long long *value) {
  size_t k = strlen(key);
  size_t i;
  long long n = 0;
  int digits = 0;

  // The query string runs from '?' to the space before "HTTP/1.x".
  for(i = 0; i < len && req[i] != '?' && req[i] != '\r'; i++);
  if(i >= len || req[i] != '?') {
    return 0;
  }

  while(i < len && req[i] != ' ' && req[i] != '\r') {
    i++;   // skip '?' or '&'
    if(i + k < len && memcmp(req + i, key, k) == 0 && req[i + k] == '=') {
      for(i += k + 1; i < len && req[i] >= '0' && req[i] <= '9'; i++) {
        if(++digits > 9) {
          return -1;
        }
        n = n * 10 + (req[i] - '0');
      }
      if(!digits || (i < len && req[i] != '&' && req[i] != ' ')) {
        return -1;
      }
      *value = n;
      return 1;
    }
    while(i < len && req[i] != '&' && req[i] != ' ' && req[i] != '\r') {
      i++;
    }
  }

  return 0;
}


static int
web_send_all(int fd, const void *buf, size_t len) {
  const char *p = buf;
  int waited = 0;
  int rc;

  while(len > 0) {
    rc = web_send(fd, p, len);
    if(rc == WEB_AGAIN) {
      if((waited += WEB_POLL_MS) > WEB_TIMEOUT_MS) {
        return -1;
      }
      web_sleep_ms(WEB_POLL_MS);
      continue;
    }
    if(rc <= 0) {
      return -1;
    }
    p += rc;
    len -= rc;
  }

  return 0;
}


static void
web_respond(int fd, const char *status, const char *type,
            const void *body, size_t len) {
  char head[256];
  int n;

  n = snprintf(head, sizeof head,
               "HTTP/1.1 %s\r\n"
               "Content-Type: %s\r\n"
               "Content-Length: %d\r\n"
               "Cache-Control: no-store\r\n"
               "Connection: close\r\n"
               "\r\n", status, type, (int)len);

  if(web_send_all(fd, head, n) == 0 && len > 0) {
    web_send_all(fd, body, len);
  }
}


static void
web_respond_json(int fd, const char *status, const char *json) {
  web_respond(fd, status, "application/json", json, strlen(json));
}


static void
web_respond_status(int fd) {
  long long now = web_uptime();
  const char *mode = !ka_active ? "off" : ka_auto ? "auto" : ka_until ? "timer" : "always";
  char remaining[24];
  char timer[24];
  char quiet[24];
  char json[768];
  char ip[32];

  if(ka_active && ka_until) {
    snprintf(remaining, sizeof remaining, "%lld", ka_until > now ? ka_until - now : 0);
    snprintf(timer, sizeof timer, "%lld", ka_minutes);
  } else {
    snprintf(remaining, sizeof remaining, "null");
    snprintf(timer, sizeof timer, "null");
  }

  // In auto mode while awake but no longer transferring: time until it rests.
  if(ka_active && ka_auto && ka_auto_awake && !ka_transferring()) {
    snprintf(quiet, sizeof quiet, "%lld", ka_quiet_left());
  } else {
    snprintf(quiet, sizeof quiet, "null");
  }

  web_get_ip(ip, sizeof ip);
  snprintf(json, sizeof json,
           "{\"version\":\"%s\",\"console\":\"%s\",\"active\":%s,\"mode\":\"%s\","
           "\"uptime\":%lld,\"since\":%lld,\"timer\":%s,\"remaining\":%s,"
           "\"net\":%s,\"rx_rate\":%lld,\"tx_rate\":%lld,\"transferring\":%s,"
           "\"auto_awake\":%s,\"quiet_left\":%s,"
           "\"interval\":%d,\"ip\":\"%s\",\"port\":%d,\"open_on_start\":%s,"
           "\"auto_threshold_kb\":%lld,\"auto_quiet_minutes\":%lld}",
           KEEPAWAKE_VERSION, WEB_CONSOLE, ka_active ? "true" : "false", mode,
           now, now - ka_changed, timer, remaining,
           ka_net_ok ? "true" : "false", ka_rx_rate, ka_tx_rate,
           ka_transferring() ? "true" : "false",
           ka_active && ka_auto && ka_auto_awake ? "true" : "false", quiet,
           KEEPAWAKE_TICK_SECONDS, ip, KEEPAWAKE_PORT,
           ka_open_on_start ? "true" : "false",
           ka_threshold_kb, ka_quiet_minutes);
  web_respond_json(fd, "200 OK", json);
}


/* POST /settings: any of open_on_start, auto_threshold_kb and
   auto_quiet_minutes. Nothing changes unless every given value is valid. */
static void
web_handle_settings(int fd, const char *req, size_t len) {
  long long open_on_start = ka_open_on_start;
  long long threshold = ka_threshold_kb;
  long long quiet = ka_quiet_minutes;
  int a = web_query_number(req, len, "open_on_start", &open_on_start);
  int b = web_query_number(req, len, "auto_threshold_kb", &threshold);
  int c = web_query_number(req, len, "auto_quiet_minutes", &quiet);

  if(a < 0 || b < 0 || c < 0 || (a + b + c) == 0 ||
     open_on_start > 1 ||
     threshold < KA_MIN_THRESHOLD_KB || threshold > KA_MAX_THRESHOLD_KB ||
     quiet < KA_MIN_QUIET_MINUTES || quiet > KA_MAX_QUIET_MINUTES) {
    web_respond_json(fd, "400 Bad Request",
                     "{\"error\":\"expected open_on_start=0|1, "
                     "auto_threshold_kb=10-102400 or auto_quiet_minutes=1-120\"}");
    return;
  }

  int old_open_on_start = ka_open_on_start;
  long long old_threshold = ka_threshold_kb;
  long long old_quiet = ka_quiet_minutes;

  ka_open_on_start = (int)open_on_start;
  ka_threshold_kb = threshold;
  ka_quiet_minutes = quiet;

  if(ka_save_settings() != 0) {
    // Keep running with what's saved, so the page and the file agree.
    ka_open_on_start = old_open_on_start;
    ka_threshold_kb = old_threshold;
    ka_quiet_minutes = old_quiet;
    web_respond_json(fd, "500 Internal Server Error",
                     "{\"error\":\"couldn't save the settings\"}");
  } else {
    web_respond_status(fd);
  }
}


/* Read the request headers, giving up after WEB_TIMEOUT_MS so a slow or idle
   client can't hold up the keep-awake loop. Returns the bytes read. */
static size_t
web_read_request(int fd, char *buf, size_t size) {
  size_t len = 0;
  int waited = 0;
  int rc;

  while(len < size - 1) {
    rc = web_recv(fd, buf + len, size - 1 - len);
    if(rc == WEB_AGAIN) {
      if((waited += WEB_POLL_MS) > WEB_TIMEOUT_MS) {
        break;
      }
      web_sleep_ms(WEB_POLL_MS);
      continue;
    }
    if(rc <= 0) {
      break;
    }
    len += rc;
    buf[len] = 0;
    if(strstr(buf, "\r\n\r\n")) {
      break;
    }
  }

  buf[len] = 0;
  return len;
}


/* Handle one client connection. Returns 1 if the payload should quit. */
static int
web_handle_client(int fd) {
  char req[1024];
  long long minutes = 0;
  long long autom = 0;
  size_t len;
  int rc;
  int rc_auto;

  if(!(len = web_read_request(fd, req, sizeof req))) {
    return 0;
  }

  if(web_route(req, len, "GET", "/") ||
     web_route(req, len, "GET", "/index.html")) {
    web_respond(fd, "200 OK", "text/html; charset=utf-8",
                index_html, index_html_len);

  } else if(web_route(req, len, "GET", "/status")) {
    web_respond_status(fd);

  } else if(web_route(req, len, "POST", "/on")) {
    rc = web_query_number(req, len, "minutes", &minutes);
    rc_auto = web_query_number(req, len, "auto", &autom);
    if(rc < 0 || minutes > KA_MAX_MINUTES || rc_auto < 0 || autom > 1 ||
       (autom && minutes)) {
      web_respond_json(fd, "400 Bad Request",
                       "{\"error\":\"use minutes=0-10080 or auto=1\"}");
    } else {
      if(autom) {
        ka_turn_on_auto();
      } else {
        ka_turn_on(minutes);
      }
      web_respond_status(fd);
    }

  } else if(web_route(req, len, "POST", "/off")) {
    ka_turn_off("Keep Awake turned off");
    web_respond_status(fd);

  } else if(web_route(req, len, "POST", "/quit")) {
    web_respond_json(fd, "200 OK", "{\"quit\":true}");
    return 1;

  } else if(web_route(req, len, "POST", "/settings")) {
    web_handle_settings(fd, req, len);

  } else if(web_route(req, len, "GET", "/on") ||
            web_route(req, len, "GET", "/off") ||
            web_route(req, len, "GET", "/quit") ||
            web_route(req, len, "GET", "/settings")) {
    // Only POST changes anything, so link previews and prefetching can't.
    web_respond_json(fd, "405 Method Not Allowed",
                     "{\"error\":\"use POST\"}");

  } else if(web_route(req, len, "GET", "/favicon.ico")) {
    web_respond(fd, "204 No Content", "text/plain", "", 0);

  } else {
    web_respond_json(fd, "404 Not Found", "{\"error\":\"not found\"}");
  }

  return 0;
}


/* --- Main loop ----------------------------------------------------------- */

/* Call regularly from the main loop: samples the network, resets the idle
   timer when it's due, and handles the timer and auto mode. Returns the
   seconds until it next needs calling. */
static int
ka_tick(void) {
  long long now = web_uptime();
  long long wait;
  int awake;

  ka_sample();

  if(ka_active && ka_until && now >= ka_until) {
    ka_turn_off("Keep Awake timer finished\nTurned off");
  }
  if(!ka_active) {
    return KA_SAMPLE_MS / 1000;
  }

  if(ka_auto) {
    awake = ka_quiet_left() > 0;
    if(awake && !ka_auto_awake) {
      ka_next_tick = 0;
      web_notify("Transfer detected\nKeeping the " WEB_CONSOLE " awake");
    } else if(!awake && ka_auto_awake) {
      web_notify("Transfers finished\nThe " WEB_CONSOLE " can rest again");
    }
    ka_auto_awake = awake;
    if(!awake) {
      return KA_SAMPLE_MS / 1000;
    }
  }

  if(now >= ka_next_tick) {
    sceSystemServicePowerTick();
    ka_next_tick = now + KEEPAWAKE_TICK_SECONDS;
  }

  wait = ka_next_tick - now;
  if(ka_until && ka_until - now < wait) {
    wait = ka_until - now;
  }
  if(wait > KA_SAMPLE_MS / 1000) {
    wait = KA_SAMPLE_MS / 1000;
  }
  return wait > 0 ? (int)wait : 1;
}


/* The request a new instance sends to make the running one quit. */
static const char web_quit_request[] =
  "POST /quit HTTP/1.0\r\n"
  "Host: localhost\r\n"
  "Content-Length: 0\r\n"
  "\r\n";
