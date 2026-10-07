/* State and tiny HTTP server for the KeepAwake control page, shared by the
   PS5 and PS4 builds.

   Keep Awake can be on (resetting the idle timer) or off (paused, payload
   still running so it can be turned back on from the page). Quitting ends
   the payload.

   Routes:
     GET  /        the control page (src/index.html)
     GET  /status  JSON status
     POST /on      start keeping the console awake
     POST /off     stop keeping it awake, but stay running
     POST /quit    end the payload

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


static int       ka_active = 1;     // resetting the idle timer?
static long long ka_changed = 0;    // web_uptime() when on/off last changed
static long long ka_next_tick = 0;  // web_uptime() when the next tick is due


static void
ka_set_active(int active) {
  if(active == ka_active) {
    return;
  }

  ka_active = active;
  ka_changed = web_uptime();
  ka_next_tick = 0;

  web_notify(active ? "Keep Awake turned on" : "Keep Awake turned off");
}


static int
web_starts_with(const char *s, size_t len, const char *prefix) {
  size_t n = strlen(prefix);
  return len >= n && memcmp(s, prefix, n) == 0;
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
  char json[320];
  char ip[32];

  web_get_ip(ip, sizeof ip);
  snprintf(json, sizeof json,
           "{\"version\":\"%s\",\"console\":\"%s\",\"active\":%s,"
           "\"uptime\":%lld,\"since\":%lld,\"interval\":%d,"
           "\"ip\":\"%s\",\"port\":%d}",
           KEEPAWAKE_VERSION, WEB_CONSOLE, ka_active ? "true" : "false",
           web_uptime(), web_uptime() - ka_changed, KEEPAWAKE_TICK_SECONDS,
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
  size_t len;

  if(!(len = web_read_request(fd, req, sizeof req))) {
    return 0;
  }

  if(web_starts_with(req, len, "GET / ") ||
     web_starts_with(req, len, "GET /index.html ")) {
    web_respond(fd, "200 OK", "text/html; charset=utf-8",
                index_html, index_html_len);

  } else if(web_starts_with(req, len, "GET /status ")) {
    web_respond_status(fd);

  } else if(web_starts_with(req, len, "POST /on ")) {
    ka_set_active(1);
    web_respond_status(fd);

  } else if(web_starts_with(req, len, "POST /off ")) {
    ka_set_active(0);
    web_respond_status(fd);

  } else if(web_starts_with(req, len, "POST /quit ")) {
    web_respond_json(fd, "200 OK", "{\"quit\":true}");
    return 1;

  } else if(web_starts_with(req, len, "GET /on ") ||
            web_starts_with(req, len, "GET /off ") ||
            web_starts_with(req, len, "GET /quit ")) {
    // Only POST changes anything, so link previews and prefetching can't.
    web_respond_json(fd, "405 Method Not Allowed",
                     "{\"error\":\"use POST\"}");

  } else if(web_starts_with(req, len, "GET /favicon.ico ")) {
    web_respond(fd, "204 No Content", "text/plain", "", 0);

  } else {
    web_respond_json(fd, "404 Not Found", "{\"error\":\"not found\"}");
  }

  return 0;
}


/* Call regularly from the main loop: resets the idle timer when it's due.
   Returns the seconds until it next needs calling. */
static int
ka_tick(void) {
  long long now = web_uptime();

  if(!ka_active) {
    return KEEPAWAKE_TICK_SECONDS;
  }
  if(now >= ka_next_tick) {
    sceSystemServicePowerTick();
    ka_next_tick = now + KEEPAWAKE_TICK_SECONDS;
  }
  return (int)(ka_next_tick - now);
}


/* The request a new instance sends to make the running one quit. */
static const char web_quit_request[] =
  "POST /quit HTTP/1.0\r\n"
  "Host: localhost\r\n"
  "Content-Length: 0\r\n"
  "\r\n";
