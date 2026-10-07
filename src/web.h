/* State and tiny HTTP server for the KeepAwake control page, shared by the
   PS5 and PS4 builds.

   Keep Awake can be on (resetting the idle timer) or off (paused, payload
   still running so it can be turned back on from the page). It can be on
   for a set time, after which it turns itself off. Quitting ends the
   payload.

   Routes:
     GET  /                  the control page (src/index.html)
     GET  /status            JSON status
     POST /on                keep the console awake with no time limit
     POST /on?minutes=N      keep it awake for N minutes, then turn off
     POST /off               stop keeping it awake, but stay running
     POST /quit              end the payload

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

/* Seconds since the payload started. */
static long long web_uptime(void);

/* The console's LAN IP address as text, or an empty string if unknown. */
static void web_get_ip(char *buf, size_t size);

/* Show a toast on the console. */
static void web_notify(const char *msg);


#define KA_MAX_MINUTES  (7 * 24 * 60)


static int       ka_active = 1;     // resetting the idle timer?
static long long ka_changed = 0;    // web_uptime() when on/off last changed
static long long ka_next_tick = 0;  // web_uptime() when the next tick is due
static long long ka_until = 0;      // web_uptime() when the timer ends, 0 if none
static long long ka_minutes = 0;    // length of the current timer


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


/* Turn on, for the given minutes or with no time limit (0). Also used to
   change or remove the timer while already on. */
static void
ka_turn_on(long long minutes) {
  int had_timer = ka_active && ka_until;
  int was_active = ka_active;
  char duration[64];
  char msg[128];

  ka_until = minutes ? web_uptime() + minutes * 60 : 0;
  ka_minutes = minutes;
  if(!ka_active) {
    ka_active = 1;
    ka_changed = web_uptime();
    ka_next_tick = 0;
  }

  if(minutes) {
    ka_format_minutes(duration, sizeof duration, minutes);
    snprintf(msg, sizeof msg, "Keep Awake on for %s", duration);
    web_notify(msg);
  } else if(!was_active) {
    web_notify("Keep Awake turned on");
  } else if(had_timer) {
    web_notify("Keep Awake timer removed\nOn with no time limit");
  }
}


static void
ka_turn_off(const char *msg) {
  if(!ka_active) {
    return;
  }

  ka_active = 0;
  ka_until = 0;
  ka_changed = web_uptime();
  web_notify(msg);
}


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
  char remaining[24];
  char timer[24];
  char json[400];
  char ip[32];

  if(ka_active && ka_until) {
    snprintf(remaining, sizeof remaining, "%lld", ka_until > now ? ka_until - now : 0);
    snprintf(timer, sizeof timer, "%lld", ka_minutes);
  } else {
    snprintf(remaining, sizeof remaining, "null");
    snprintf(timer, sizeof timer, "null");
  }

  web_get_ip(ip, sizeof ip);
  snprintf(json, sizeof json,
           "{\"version\":\"%s\",\"console\":\"%s\",\"active\":%s,"
           "\"uptime\":%lld,\"since\":%lld,\"timer\":%s,\"remaining\":%s,"
           "\"interval\":%d,\"ip\":\"%s\",\"port\":%d}",
           KEEPAWAKE_VERSION, WEB_CONSOLE, ka_active ? "true" : "false",
           now, now - ka_changed, timer, remaining, KEEPAWAKE_TICK_SECONDS,
           ip, KEEPAWAKE_PORT);
  web_respond_json(fd, "200 OK", json);
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
  size_t len;
  int rc;

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
    if(rc < 0 || minutes > KA_MAX_MINUTES) {
      web_respond_json(fd, "400 Bad Request",
                       "{\"error\":\"minutes must be a whole number from 0 to 10080\"}");
    } else {
      ka_turn_on(minutes);
      web_respond_status(fd);
    }

  } else if(web_route(req, len, "POST", "/off")) {
    ka_turn_off("Keep Awake turned off");
    web_respond_status(fd);

  } else if(web_route(req, len, "POST", "/quit")) {
    web_respond_json(fd, "200 OK", "{\"quit\":true}");
    return 1;

  } else if(web_route(req, len, "GET", "/on") ||
            web_route(req, len, "GET", "/off") ||
            web_route(req, len, "GET", "/quit")) {
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


/* Call regularly from the main loop: resets the idle timer when it's due and
   turns off when the timer ends. Returns the seconds until it next needs
   calling. */
static int
ka_tick(void) {
  long long now = web_uptime();
  long long wait;

  if(ka_active && ka_until && now >= ka_until) {
    ka_turn_off("Keep Awake timer finished\nTurned off");
  }
  if(!ka_active) {
    return KEEPAWAKE_TICK_SECONDS;
  }

  if(now >= ka_next_tick) {
    sceSystemServicePowerTick();
    ka_next_tick = now + KEEPAWAKE_TICK_SECONDS;
  }

  wait = ka_next_tick - now;
  if(ka_until && ka_until - now < wait) {
    wait = ka_until - now;
  }
  return wait > 0 ? (int)wait : 1;
}


/* The request a new instance sends to make the running one quit. */
static const char web_quit_request[] =
  "POST /quit HTTP/1.0\r\n"
  "Host: localhost\r\n"
  "Content-Length: 0\r\n"
  "\r\n";
