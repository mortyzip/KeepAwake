/* KeepAwake (PS4) - keep the PS4 out of rest mode while this payload is running.

   Same behaviour as the PS5 build: the idle timer is reset by periodically
   calling sceSystemServicePowerTick(), and a local TCP port makes the
   payload a toggle (sending it again stops the running instance).

   libPS4 has no select(), so the control socket is non-blocking and polled
   once a second. */

#include "ps4.h"


#ifndef KEEPAWAKE_VERSION
#define KEEPAWAKE_VERSION       "dev"
#endif

#define KEEPAWAKE_PORT          9031
#define KEEPAWAKE_TICK_SECONDS  10


static int (*sceSystemServicePowerTick)(void);


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


static void
make_addr(struct sockaddr_in *addr, unsigned int ip) {
  memset(addr, 0, sizeof *addr);
  addr->sin_len = sizeof *addr;
  addr->sin_family = AF_INET;
  addr->sin_port = htons(KEEPAWAKE_PORT);
  addr->sin_addr.s_addr = ip;
}


/* Ask an already running instance to stop by connecting to its port. */
static int
stop_running_instance(void) {
  struct sockaddr_in addr;
  int fd;
  int rc;

  if((fd = sceNetSocket("keepawake_stop", AF_INET, SOCK_STREAM, 0)) < 0) {
    return -1;
  }

  make_addr(&addr, IP(127, 0, 0, 1));
  rc = sceNetConnect(fd, (struct sockaddr*)&addr, sizeof addr);
  sceNetSocketClose(fd);

  return rc < 0 ? -1 : 0;
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
     sceNetListen(fd, 1) < 0) {
    sceNetSocketClose(fd);
    return -1;
  }

  return fd;
}


int
_main(struct thread *td) {
  int srv;
  int cli;

  UNUSED(td);

  initKernel();
  initLibc();
  initNetwork();
  jailbreak();

  if(resolve_power_tick() != 0) {
    printf_notification("Keep Awake failed to start:\n"
                        "sceSystemServicePowerTick not found");
    return -1;
  }

  if((srv = open_control_socket()) < 0) {
    // Port taken: an instance is already running, so toggle it off.
    // The running instance shows the "disabled" toast itself.
    if(stop_running_instance() == 0) {
      return 0;
    }
    printf_notification("Keep Awake failed to start:\n"
                        "port %d unavailable", KEEPAWAKE_PORT);
    return -1;
  }

  printf_notification("Keep Awake " KEEPAWAKE_VERSION " enabled\n"
                      "Send the payload again to disable");

  while(1) {
    sceSystemServicePowerTick();

    for(int i = 0; i < KEEPAWAKE_TICK_SECONDS; i++) {
      // Any incoming connection is a request to stop.
      if((cli = sceNetAccept(srv, NULL, NULL)) >= 0) {
        sceNetSocketClose(cli);
        printf_notification("Keep Awake " KEEPAWAKE_VERSION " disabled");
        sceNetSocketClose(srv);
        return 0;
      }
      sceKernelSleep(1);
    }
  }
}
