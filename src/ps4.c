/* KeepAwake (PS4) - keep the PS4 out of rest mode while this payload is running.

   Same behaviour as the PS5 build: the idle timer is reset by periodically
   calling sceSystemServicePowerTick(), and a control page is served on TCP
   port 9031 (see web.h).

   libPS4 has no select(), so the control socket is non-blocking and polled
   ten times a second. */

#include "ps4.h"


#ifndef KEEPAWAKE_VERSION
#define KEEPAWAKE_VERSION       "dev"
#endif

#define KEEPAWAKE_PORT          9031
#define KEEPAWAKE_TICK_SECONDS  10
#define KEEPAWAKE_POLL_MS       100

#define SCE_NET_SO_RCVTIMEO     0x1006
#define SCE_NET_EAGAIN          35
#define SCE_NET_ERROR_EAGAIN    0x80410123


static int (*sceSystemServicePowerTick)(void);

static uint64_t g_started;


static int
resolve_power_tick(void) {
  int handle = sceKernelLoadStartModule("/system/common/lib/libSceSystemService.sprx",
                                        0, 0, 0, NULL, NULL);
  if(handle < 0) {
    return -1;
  }

  RESOLVE(handle, sceSystemServicePowerTick);
  return sceSystemServicePowerTick ? 0 : -1;
}


static int
would_block(int rc) {
  return (unsigned int)rc == SCE_NET_ERROR_EAGAIN || sce_net_errno == SCE_NET_EAGAIN;
}


#define WEB_CONSOLE "PS4"
#include "web.h"

static int
web_recv(int fd, void *buf, size_t len) {
  int rc = sceNetRecv(fd, buf, len, 0);
  if(rc < 0) {
    return would_block(rc) ? WEB_AGAIN : WEB_ERROR;
  }
  return rc;
}

static int
web_send(int fd, const void *buf, size_t len) {
  int rc = sceNetSend(fd, buf, len, 0);
  if(rc < 0) {
    return would_block(rc) ? WEB_AGAIN : WEB_ERROR;
  }
  return rc;
}

static void
web_sleep_ms(int ms) {
  sceKernelUsleep(ms * 1000);
}

static long long
web_uptime(void) {
  return (sceKernelGetProcessTime() - g_started) / 1000000;
}

static void
web_get_ip(char *buf, size_t size) {
  SceNetCtlInfo info;

  buf[0] = 0;
  memset(&info, 0, sizeof info);
  if(sceNetCtlGetInfo(SCE_NET_CTL_INFO_IP_ADDRESS, &info) == 0) {
    snprintf(buf, size, "%s", info.ip_address);
  }
}

static void
web_notify(const char *msg) {
  printf_notification("%s", msg);
}


static void
make_addr(struct sockaddr_in *addr, unsigned int ip) {
  memset(addr, 0, sizeof *addr);
  addr->sin_len = sizeof *addr;
  addr->sin_family = AF_INET;
  addr->sin_port = htons(KEEPAWAKE_PORT);
  addr->sin_addr.s_addr = ip;
}


/* Ask an already running instance to quit. Returns 0 if it confirmed. */
static int
quit_running_instance(void) {
  struct sockaddr_in addr;
  int timeout_us = 3 * 1000 * 1000;
  char resp[64];
  int fd;
  int n;

  if((fd = sceNetSocket("keepawake_quit", AF_INET, SOCK_STREAM, 0)) < 0) {
    return -1;
  }

  sceNetSetsockopt(fd, SOL_SOCKET, SCE_NET_SO_RCVTIMEO, &timeout_us, sizeof timeout_us);

  make_addr(&addr, IP(127, 0, 0, 1));
  if(sceNetConnect(fd, (struct sockaddr*)&addr, sizeof addr) < 0 ||
     sceNetSend(fd, web_quit_request, sizeof web_quit_request - 1, 0) < 0) {
    sceNetSocketClose(fd);
    return -1;
  }

  n = sceNetRecv(fd, resp, sizeof resp - 1, 0);
  sceNetSocketClose(fd);

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

  if((fd = sceNetSocket("keepawake", AF_INET, SOCK_STREAM, 0)) < 0) {
    return -1;
  }

  sceNetSetsockopt(fd, SOL_SOCKET, SCE_NET_SO_REUSEADDR, &yes, sizeof yes);
  sceNetSetsockopt(fd, SOL_SOCKET, SO_NBIO, &yes, sizeof yes);

  make_addr(&addr, IN_ADDR_ANY);
  if(sceNetBind(fd, (struct sockaddr*)&addr, sizeof addr) < 0 ||
     sceNetListen(fd, 8) < 0) {
    sceNetSocketClose(fd);
    return -1;
  }

  return fd;
}


int
_main(struct thread *td) {
  char ip[32];
  int yes = 1;
  int srv;
  int cli;
  int quit;

  UNUSED(td);

  initKernel();
  initLibc();
  initNetwork();
  jailbreak();
  sceNetCtlInit();

  if(resolve_power_tick() != 0) {
    printf_notification("Keep Awake failed to start:\n"
                        "sceSystemServicePowerTick not found");
    return -1;
  }

  if((srv = open_control_socket()) < 0) {
    // Port taken: an instance is already running, so close it.
    // The running instance shows the "closed" toast itself.
    if(quit_running_instance() == 0) {
      return 0;
    }
    printf_notification("Keep Awake failed to start:\n"
                        "port %d is in use", KEEPAWAKE_PORT);
    return -1;
  }

  g_started = sceKernelGetProcessTime();

  web_get_ip(ip, sizeof ip);
  if(ip[0]) {
    printf_notification("Keep Awake " KEEPAWAKE_VERSION " enabled\n"
                        "Control it at http://%s:%d", ip, KEEPAWAKE_PORT);
  } else {
    printf_notification("Keep Awake " KEEPAWAKE_VERSION " enabled\n"
                        "Control page on port %d", KEEPAWAKE_PORT);
  }

  while(1) {
    ka_tick();

    if((cli = sceNetAccept(srv, NULL, NULL)) >= 0) {
      sceNetSetsockopt(cli, SOL_SOCKET, SO_NBIO, &yes, sizeof yes);
      quit = web_handle_client(cli);
      sceNetSocketClose(cli);
      if(quit) {
        printf_notification("Keep Awake " KEEPAWAKE_VERSION " closed");
        sceNetSocketClose(srv);
        return 0;
      }
      continue;
    }

    sceKernelUsleep(KEEPAWAKE_POLL_MS * 1000);
  }
}
