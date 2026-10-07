/* Stand-ins for the PS5 system calls, so src/ps5.c can run on a computer
   (see `make host`). */

#include <stdio.h>
#include <stddef.h>


int
sceKernelSendNotificationRequest(int device, void *req, size_t size, int blocking) {
  return 0;
}


int
sceSystemServicePowerTick(void) {
  printf("[KeepAwake] power tick\n");
  fflush(stdout);
  return 0;
}
