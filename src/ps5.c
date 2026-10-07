/* KeepAwake - keep the PS5 out of rest mode while this payload is running.

   The console's idle timer is reset by periodically calling
   sceSystemServicePowerTick(). The payload also serves a control page on
   TCP port 9031 (see web.h), which
     - shows the status and turns Keep Awake on and off from a browser
       (and opens in the PS5's browser at start, unless turned off),
     - guarantees only one instance runs at a time, and
     - lets a second copy of the payload close the running one. */

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <netinet/in.h>
#include <pthread.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>


#ifndef KEEPAWAKE_VERSION
#define KEEPAWAKE_VERSION       "dev"
#endif

#define KEEPAWAKE_PORT          9031
#define KEEPAWAKE_TICK_SECONDS  10


typedef struct notify_request {
  char useless1[45];
  char message[3075];
} notify_request_t;


int sceKernelSendNotificationRequest(int, notify_request_t*, size_t, int);
int sceSystemServicePowerTick(void);
int sceSystemServiceLaunchWebBrowser(const char *uri, void *param);
int sceUserServiceInitialize(void *param);


static time_t g_started;


static time_t
now_seconds(void) {
  struct timespec ts;

  clock_gettime(CLOCK_MONOTONIC, &ts);
  return ts.tv_sec;
}


static void
notify(const char *fmt, ...) {
  notify_request_t req;
  va_list args;

  bzero(&req, sizeof req);
  va_start(args, fmt);
  vsnprintf(req.message, sizeof req.message, fmt, args);
  va_end(args);

  printf("[KeepAwake] %s\n", req.message);
  fflush(stdout);
  sceKernelSendNotificationRequest(0, &req, sizeof req, 0);
}


#define WEB_CONSOLE "PS5"
#include "web.h"

static int
web_recv(int fd, void *buf, size_t len) {
  ssize_t rc = recv(fd, buf, len, 0);
  if(rc < 0) {
    return (errno == EAGAIN || errno == EWOULDBLOCK) ? WEB_AGAIN : WEB_ERROR;
  }
  return rc;
}

static int
web_send(int fd, const void *buf, size_t len) {
  ssize_t rc = send(fd, buf, len, 0);
  if(rc < 0) {
    return (errno == EAGAIN || errno == EWOULDBLOCK) ? WEB_AGAIN : WEB_ERROR;
  }
  return rc;
}

static void
web_sleep_ms(int ms) {
  usleep(ms * 1000);
}

static long long
web_uptime(void) {
  return now_seconds() - g_started;
}

static void
web_get_ip(char *buf, size_t size) {
  struct ifaddrs *ifs;
  struct ifaddrs *it;

  buf[0] = 0;
  if(getifaddrs(&ifs) != 0) {
    return;
  }

  for(it = ifs; it; it = it->ifa_next) {
    if(!it->ifa_addr || it->ifa_addr->sa_family != AF_INET ||
       !(it->ifa_flags & IFF_UP) || (it->ifa_flags & IFF_LOOPBACK)) {
      continue;
    }
    inet_ntop(AF_INET, &((struct sockaddr_in*)it->ifa_addr)->sin_addr,
              buf, size);
    break;
  }

  freeifaddrs(ifs);
}

static void
web_notify(const char *msg) {
  notify("%s", msg);
}


/* Runs on its own thread, so the control page is served even if launching
   the browser blocks. */
static void*
open_browser_thread(void *arg) {
  char url[64];

  snprintf(url, sizeof url, "http://127.0.0.1:%d/", KEEPAWAKE_PORT);
  sceUserServiceInitialize(0);
  if(sceSystemServiceLaunchWebBrowser(url, 0) != 0) {
    notify("Keep Awake couldn't open the browser");
  }
  return NULL;
}


static void
open_browser(void) {
  pthread_t thread;

  if(pthread_create(&thread, NULL, open_browser_thread, NULL) == 0) {
    pthread_detach(thread);
  }
}


/* Ask an already running instance to quit. Returns 0 if it confirmed. */
static int
quit_running_instance(void) {
  struct sockaddr_in addr;
  struct timeval tv = {3, 0};
  char resp[64];
  ssize_t n;
  int fd;

  if((fd = socket(AF_INET, SOCK_STREAM, 0)) < 0) {
    return -1;
  }

  setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);

  bzero(&addr, sizeof addr);
  addr.sin_family = AF_INET;
  addr.sin_port = htons(KEEPAWAKE_PORT);
  addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);

  if(connect(fd, (struct sockaddr*)&addr, sizeof addr) != 0 ||
     send(fd, web_quit_request, sizeof web_quit_request - 1, 0) < 0) {
    close(fd);
    return -1;
  }

  n = recv(fd, resp, sizeof resp - 1, 0);
  close(fd);

  if(n <= 0) {
    return -1;
  }
  resp[n] = 0;
  return strstr(resp, " 200 ") ? 0 : -1;
}


static int
open_control_socket(void) {
  struct sockaddr_in addr;
  int yes = 1;
  int fd;

  if((fd = socket(AF_INET, SOCK_STREAM, 0)) < 0) {
    return -1;
  }

  setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof yes);

  bzero(&addr, sizeof addr);
  addr.sin_family = AF_INET;
  addr.sin_port = htons(KEEPAWAKE_PORT);
  addr.sin_addr.s_addr = htonl(INADDR_ANY);

  if(bind(fd, (struct sockaddr*)&addr, sizeof addr) != 0 ||
     listen(fd, 8) != 0) {
    int err = errno;
    close(fd);
    errno = err;
    return -1;
  }

  return fd;
}


int
main(void) {
  struct timeval tv;
  char ip[32];
  fd_set fds;
  int srv;
  int cli;
  int rc;

  signal(SIGPIPE, SIG_IGN);

  if((srv = open_control_socket()) < 0) {
    if(errno == EADDRINUSE) {
      // Port taken: an instance is already running, so close it.
      // The running instance shows the "closed" toast itself.
      if(quit_running_instance() == 0) {
        return 0;
      }
      notify("Keep Awake failed to start:\nport %d is in use", KEEPAWAKE_PORT);
      return -1;
    }
    notify("Keep Awake failed to start: %s", strerror(errno));
    return -1;
  }

  g_started = now_seconds();
  ka_load_settings();

  web_get_ip(ip, sizeof ip);
  if(ip[0]) {
    notify("Keep Awake " KEEPAWAKE_VERSION " enabled\n"
           "Control it at http://%s:%d", ip, KEEPAWAKE_PORT);
  } else {
    notify("Keep Awake " KEEPAWAKE_VERSION " enabled\n"
           "Control page on port %d", KEEPAWAKE_PORT);
  }

  if(ka_open_on_start) {
    open_browser();
  }

  while(1) {
    FD_ZERO(&fds);
    FD_SET(srv, &fds);
    tv.tv_sec = ka_tick();
    tv.tv_usec = 0;

    rc = select(srv + 1, &fds, NULL, NULL, &tv);
    if(rc < 0) {
      if(errno == EINTR) {
        continue;
      }
      notify("Keep Awake stopped: %s", strerror(errno));
      break;
    }

    if(rc > 0 && FD_ISSET(srv, &fds)) {
      if((cli = accept(srv, NULL, NULL)) < 0) {
        continue;
      }
      fcntl(cli, F_SETFL, fcntl(cli, F_GETFL) | O_NONBLOCK);
      rc = web_handle_client(cli);
      close(cli);
      if(rc) {
        notify("Keep Awake " KEEPAWAKE_VERSION " closed");
        break;
      }
    }
  }

  close(srv);
  return 0;
}
