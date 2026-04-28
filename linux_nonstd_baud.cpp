// Linux-only: termios2/BOTHER for non-standard baud.
// Avoid glibc <termios.h> / <sys/ioctl.h> together with <linux/termios.h> (duplicate termios/winsize).
// Use kernel UAPI termbits + ioctl numbers only, and declare ioctl(3) — no ioctl-types.h winsize.
#if defined(__linux__)

#include <asm/termbits.h>
#include <asm/ioctls.h>

#include <cstdint>

extern "C" int ioctl(int fd, unsigned long request, ...);

bool linux_set_nonstandard_baud(int fd, uint32_t baud) {
  struct termios2 t2;
  if (ioctl(fd, TCGETS2, &t2) != 0) {
    return false;
  }
  t2.c_cflag &= static_cast<tcflag_t>(~CBAUD);
  t2.c_cflag |= BOTHER;
  t2.c_ispeed = baud;
  t2.c_ospeed = baud;
  return ioctl(fd, TCSETS2, &t2) == 0;
}

#endif
