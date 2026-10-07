/* KeepAwake - keep the PS5 out of rest mode while this payload is running.

   The console's idle timer is reset by periodically calling
   sceSystemServicePowerTick(). The payload also listens on a local TCP
   port, which serves two purposes:
     - it guarantees only one instance runs at a time, and
     - sending the payload a second time (or connecting to the port)
       stops the running instance, i.e. the payload works as a toggle. */

#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <sys/select.h>
#include <sys/socket.h>
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


static void
notify(const char *fmt, ...) {
  notify_request_t req;
  va_list args;

  bzero(&req, sizeof req);
  va_start(args, fmt);
  vsnprintf(req.message, sizeof req.message, fmt, args);
  va_end(args);

  printf("[KeepAwake] %s\n", req.message);
  sceKernelSendNotificationRequest(0, &req, sizeof req, 0);
}


/* Ask an already running instance to stop by connecting to its port. */
static int
stop_running_instance(void) {
  struct sockaddr_in addr;
  int fd;
  int rc;

  if((fd = socket(AF_INET, SOCK_STREAM, 0)) < 0) {
    return -1;
  }

  bzero(&addr, sizeof addr);
  addr.sin_family = AF_INET;
  addr.sin_port = htons(KEEPAWAKE_PORT);
  addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);

  rc = connect(fd, (struct sockaddr*)&addr, sizeof addr);
  close(fd);

  return rc;
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
     listen(fd, 1) != 0) {
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
  fd_set fds;
  int srv;
  int rc;

  signal(SIGPIPE, SIG_IGN);

  if((srv = open_control_socket()) < 0) {
    if(errno == EADDRINUSE) {
      // Port taken: an instance is already running, so toggle it off.
      // The running instance shows the "disabled" toast itself.
      if(stop_running_instance() == 0) {
        return 0;
      }
    }
    notify("Keep Awake failed to start: %s", strerror(errno));
    return -1;
  }

  notify("Keep Awake " KEEPAWAKE_VERSION " enabled\n"
         "Send the payload again to disable");

  while(1) {
    sceSystemServicePowerTick();

    FD_ZERO(&fds);
    FD_SET(srv, &fds);
    tv.tv_sec = KEEPAWAKE_TICK_SECONDS;
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
      // Any incoming connection is a request to stop.
      int cli = accept(srv, NULL, NULL);
      if(cli >= 0) {
        close(cli);
      }
      notify("Keep Awake " KEEPAWAKE_VERSION " disabled");
      break;
    }
  }

  close(srv);
  return 0;
}
