// Linux-only: termios2/BOTHER for non-standard baud. Must not pull in <termios.h> in the same TU as
// <linux/termios.h> — glibc and kernel UAPI both define struct termios / winsize and conflict.
#if defined(__linux__)

#include <sys/ioctl.h>
#include <linux/termios.h>

#include <cstdint>

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
