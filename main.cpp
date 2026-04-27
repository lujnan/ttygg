// ttygg: serial monitor with line filtering (macOS / Linux, poll + termios)
#include <algorithm>
#include <cerrno>
#include <cstdarg>
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <cstdlib>
#include <iostream>
#include <regex>
#include <string>
#include <vector>

#include <fcntl.h>
#include <glob.h>
#include <poll.h>
#include <sys/ioctl.h>
#include <termios.h>
#include <unistd.h>

#if defined(__linux__)
#include <linux/termios.h>
#endif
#if defined(__APPLE__)
#include <IOKit/serial/ioss.h>
#endif

namespace {

bool write_all(int fd, const char *data, size_t len);

struct Options {
  std::string port;
  uint32_t baud = 115200;
  std::vector<std::string> exclude_patterns;
  bool list_ports = false;
  bool help = false;
  /// Trace state to stderr (does not mix with serial data on stdout).
  bool verbose = false;
  /// Do not flush partial serial buffer on poll timeout (safer for nsh; long lines without \\n wait for more data).
  bool no_idle_flush = false;
  /// If non-empty, append a text log of wire TX/RX (see --log).
  std::string log_path;
  /// Echo a copy of bytes we send to the serial to stdout (see --local-echo).
  bool local_echo = false;
};

struct LocalTermiosGuard {
  int fd = -1;
  struct termios saved {};
  bool armed = false;

  LocalTermiosGuard() = default;
  ~LocalTermiosGuard() {
    if (armed && fd >= 0) {
      (void)tcsetattr(fd, TCSADRAIN, &saved);
    }
  }
  bool try_arm(int stdin_fd) {
    fd = stdin_fd;
    if (!isatty(fd)) {
      return true;
    }
    if (tcgetattr(fd, &saved) != 0) {
      return false;
    }
    struct termios t = saved;
    t.c_lflag &= static_cast<tcflag_t>(~(ICANON | ECHO | ECHOE | ECHONL | ISIG));
    /* IXON/IXOFF: driver may take ^Q/^S before read(); must clear for Ctrl+A,Ctrl+Q exit and -i. */
    t.c_iflag &= static_cast<tcflag_t>(~(IXON | IXOFF));
    t.c_cc[VMIN] = 1;
    t.c_cc[VTIME] = 0;
    if (tcsetattr(fd, TCSANOW, &t) != 0) {
      return false;
    }
    armed = true;
    return true;
  }
  LocalTermiosGuard(const LocalTermiosGuard &) = delete;
  LocalTermiosGuard &operator=(const LocalTermiosGuard &) = delete;
};

void tlog(bool verbose, const char *fmt, ...) {
  if (!verbose) {
    return;
  }
  std::fputs("[ttygg] ", stderr);
  va_list ap;
  va_start(ap, fmt);
  std::vfprintf(stderr, fmt, ap);
  va_end(ap);
  std::fputc('\n', stderr);
  std::fflush(stderr);
}

void tlog_serial_read_hex(bool verbose, const uint8_t *p, size_t n) {
  if (!verbose || n == 0) {
    return;
  }
  const size_t cap = 64;
  const size_t show = n > cap ? cap : n;
  std::fprintf(stderr, "[ttygg] serial read %zu bytes, first %zu hex:", n, show);
  for (size_t i = 0; i < show; i++) {
    std::fprintf(stderr, " %02x", static_cast<unsigned>(p[i]));
  }
  if (n > cap) {
    std::fprintf(stderr, " ...");
  }
  std::fprintf(stderr, "\n");
  std::fflush(stderr);
}

void append_escaped_to(std::string *out, const unsigned char *b, size_t n) {
  for (size_t i = 0; i < n; i++) {
    const unsigned c = b[i];
    switch (c) {
    case 0:
      out->append("\\0");
      break;
    case '\a':
      out->append("\\a");
      break;
    case '\b':
      out->append("\\b");
      break;
    case '\t':
      out->append("\\t");
      break;
    case '\n':
      out->append("\\n");
      break;
    case '\v':
      out->append("\\v");
      break;
    case '\f':
      out->append("\\f");
      break;
    case '\r':
      out->append("\\r");
      break;
    case '\\':
      out->append("\\\\");
      break;
    default:
      if (c >= 32U && c < 127U) {
        out->push_back(static_cast<char>(c));
      } else if (c >= 128U) {
        /* UTF-8 and high bytes: keep as-is so log stays readable in UTF-8 locales */
        out->push_back(static_cast<char>(c));
      } else {
        char buf[8];
        (void)std::snprintf(buf, sizeof buf, "\\x%02x", c);
        out->append(buf);
      }
    }
  }
}

void log_serial_wire(int log_fd, const void *p, size_t n) {
  if (log_fd < 0 || n == 0) {
    return;
  }
  const auto *b = static_cast<const unsigned char *>(p);
  std::string line;
  line.reserve(n * 2 + 8U);
  append_escaped_to(&line, b, n);
  line.push_back('\n');
  (void)write_all(log_fd, line.data(), line.size());
}

struct LogFileGuard {
  int fd;
  explicit LogFileGuard(int f) : fd(f) {}
  ~LogFileGuard() {
    if (fd >= 0) {
      (void)close(fd);
    }
  }
  LogFileGuard(const LogFileGuard &) = delete;
  LogFileGuard &operator=(const LogFileGuard &) = delete;
};

void print_help(const char *argv0) {
  std::cerr
      << "Usage: " << argv0 << " [options]\n"
      << "Interactive serial monitor (cbreak stdin, per-key to port, picocom-style exit). Example:\n"
      << "  " << argv0 << " -p /dev/cu.usbserial-0 -b 921600 -e '...' -L session.log\n"
      << "\n"
      << "  -p, --port DEVICE     Serial device. On macOS use /dev/cu.* (call-out); /dev/tty.* can block in open(2)\n"
      << "                          Linux e.g. /dev/ttyUSB0, /dev/ttyACM0\n"
      << "  -b, --baud RATE        Baud rate (default: 115200)\n"
      << "  -e, --exclude PATTERN   ECMAScript regex; lines matching any pattern are not printed (repeatable).\n"
      << "                          With no -e, serial RX is written to stdout as it arrives; with -e, RX is line-based.\n"
      << "  -l, --list              List likely serial device paths and exit\n"
      << "  -h, --help              Show this help\n"
      << "      --local-echo        Also print to stdout a copy of everything sent to the serial (if the target does not echo).\n"
      << "  -v, --verbose           Trace run state to stderr. Env TTYGG_DEBUG=1 enables the same.\n"
      << "      --no-idle-flush     With -e: do not flush partial serial→stdout (neither after serial drain nor on poll timeout). TTYGG_NO_IDLE=1 same.\n"
      << "  -L, --log FILE          Append wire traffic to FILE (escaped text; see previous versions for format).\n"
      << "\n"
      << "Stdin is a TTY: cbreak, no local echo, ISIG off, IXON off — each key goes to the port. Add --local-echo if blind.\n"
      << "Exit: Ctrl+A then Ctrl+Q or Ctrl+X (prefix, then quit). Double Ctrl+A sends one literal 0x01 to the port.\n"
      << "With -e, logical lines end on \\n for matching; \\r before \\n stripped. Partial flushes (line editing) run after each serial read batch. If a long line is split across USB reads, a matching exclude is tracked until a \\n so tails are not re-printed; rare false drops if unrelated text follows the same split.\n"
      << "Order: serial to the terminal is applied before the next key is sent, so nsh/line editing redraws stay aligned.\n"
      << "Non-TTY stdin: no cbreak; keyboard path is best-effort. High non-standard bauds use OS ioctls.\n"
      << "Legacy flags -i / --interactive are accepted and ignored.\n";
}

bool parse_u32(const char *s, uint32_t *out) {
  char *end = nullptr;
  unsigned long v = std::strtoul(s, &end, 10);
  if (end == s || *end != '\0' || v == 0)
    return false;
  *out = static_cast<uint32_t>(v);
  return true;
}

bool parse_args(int argc, char **argv, Options *opt, std::string *err) {
  for (int i = 1; i < argc; i++) {
    const char *a = argv[i];
    if (std::strcmp(a, "-h") == 0 || std::strcmp(a, "--help") == 0) {
      opt->help = true;
      continue;
    }
    if (std::strcmp(a, "-l") == 0 || std::strcmp(a, "--list") == 0) {
      opt->list_ports = true;
      continue;
    }
    if (std::strcmp(a, "-p") == 0 || std::strcmp(a, "--port") == 0) {
      if (i + 1 >= argc) {
        *err = "Missing value for --port";
        return false;
      }
      opt->port = argv[++i];
      continue;
    }
    if (std::strcmp(a, "-b") == 0 || std::strcmp(a, "--baud") == 0) {
      if (i + 1 >= argc) {
        *err = "Missing value for --baud";
        return false;
      }
      if (!parse_u32(argv[++i], &opt->baud)) {
        *err = "Invalid baud rate";
        return false;
      }
      continue;
    }
    if (std::strcmp(a, "-e") == 0 || std::strcmp(a, "--exclude") == 0) {
      if (i + 1 >= argc) {
        *err = "Missing value for --exclude";
        return false;
      }
      opt->exclude_patterns.push_back(argv[++i]);
      continue;
    }
    if (std::strcmp(a, "-i") == 0 || std::strcmp(a, "--interactive") == 0) {
      continue; /* historic no-op: ttygg is always interactive */
    }
    if (std::strcmp(a, "--raw") == 0) {
      *err = "ttygg: --raw is removed; behaviour is the former -i (interactive) only";
      return false;
    }
    if (std::strcmp(a, "--eol") == 0) {
      *err = "ttygg: --eol is removed; keys are sent as the terminal provides them (Enter often sends ^M or ^J)";
      if (i + 1 < argc && argv[i + 1][0] != '-') {
        ++i;
      }
      return false;
    }
    if (std::strcmp(a, "--local-echo") == 0) {
      opt->local_echo = true;
      continue;
    }
    if (std::strcmp(a, "--no-idle-flush") == 0) {
      opt->no_idle_flush = true;
      continue;
    }
    if (std::strcmp(a, "-v") == 0 || std::strcmp(a, "--verbose") == 0) {
      opt->verbose = true;
      continue;
    }
    if (std::strcmp(a, "-L") == 0 || std::strcmp(a, "--log") == 0) {
      if (i + 1 >= argc) {
        *err = "Missing value for --log";
        return false;
      }
      opt->log_path = argv[++i];
      continue;
    }
    *err = std::string("Unknown option: ") + a;
    return false;
  }
  if (opt->list_ports) {
    return true;
  }
  if (opt->help) {
    return true;
  }
  if (opt->port.empty()) {
    *err = "Serial port is required (use -p or --port)";
    return false;
  }
  return true;
}

speed_t baud_to_termios_speed(uint32_t baud) {
  switch (baud) {
  case 50:
    return B50;
  case 75:
    return B75;
  case 110:
    return B110;
  case 134:
    return B134;
  case 150:
    return B150;
  case 200:
    return B200;
  case 300:
    return B300;
  case 600:
    return B600;
  case 1200:
    return B1200;
  case 1800:
    return B1800;
  case 2400:
    return B2400;
  case 4800:
    return B4800;
  case 9600:
    return B9600;
  case 19200:
    return B19200;
  case 38400:
    return B38400;
  case 57600:
    return B57600;
  case 115200:
    return B115200;
  case 230400:
    return B230400;
#ifdef B460800
  case 460800:
    return B460800;
#endif
#ifdef B500000
  case 500000:
    return B500000;
#endif
#ifdef B576000
  case 576000:
    return B576000;
#endif
#ifdef B921600
  case 921600:
    return B921600;
#endif
#ifdef B1000000
  case 1000000:
    return B1000000;
#endif
#ifdef B1152000
  case 1152000:
    return B1152000;
#endif
#ifdef B1500000
  case 1500000:
    return B1500000;
#endif
#ifdef B2000000
  case 2000000:
    return B2000000;
#endif
#ifdef B2500000
  case 2500000:
    return B2500000;
#endif
#ifdef B3000000
  case 3000000:
    return B3000000;
#endif
#ifdef B3500000
  case 3500000:
    return B3500000;
#endif
#ifdef B4000000
  case 4000000:
    return B4000000;
#endif
  default:
    return 0;
  }
}

#if defined(__linux__)
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

#if defined(__APPLE__)
bool macos_set_nonstandard_baud(int fd, uint32_t baud) {
  speed_t s = static_cast<speed_t>(baud);
  return ioctl(fd, IOSSIOSPEED, &s) == 0;
}
#endif

int open_serial_port(const char *path, uint32_t baud, bool verbose) {
  /* macOS: /dev/tty.* is the dial-in side; open(2) often blocks until DCD or similar. Programs that
   * "call out" must use the matching /dev/cu.* (call-out) node, or open on tty can hang "forever". */
  std::string path_apple_fix;
  const char *open_path = path;
#if defined(__APPLE__)
  if (std::strncmp(path, "/dev/tty.", 9) == 0) {
    path_apple_fix = std::string("/dev/cu.") + (path + 9);
    open_path = path_apple_fix.c_str();
    tlog(verbose, "macOS: %s can block in open(2); using call-out %s", path, open_path);
  }
#endif
  tlog(verbose, "open %s: attempting O_RDWR|O_NOCTTY", open_path);
  int fd = open(open_path, O_RDWR | O_NOCTTY);
  if (fd < 0) {
    std::cerr << "open " << open_path << ": " << std::strerror(errno) << "\n";
    return -1;
  }
  tlog(verbose, "open ok, fd=%d", fd);

  speed_t sp = baud_to_termios_speed(baud);
  speed_t termios_speed = (sp != 0) ? sp : B115200;
  if (sp != 0) {
    tlog(verbose, "baud: using POSIX speed index (B*) for value %u", (unsigned)baud);
  } else {
    tlog(verbose, "baud: no B* for %u, will set placeholder then OS-specific ioctl", (unsigned)baud);
  }

  struct termios tio;
  if (tcgetattr(fd, &tio) != 0) {
    std::cerr << "tcgetattr: " << std::strerror(errno) << "\n";
    close(fd);
    return -1;
  }

  cfmakeraw(&tio);
  if (cfsetspeed(&tio, termios_speed) != 0) {
    std::cerr << "cfsetspeed: " << std::strerror(errno) << "\n";
    close(fd);
    return -1;
  }
  tio.c_cflag |= (CLOCAL | CREAD);
  tio.c_cflag &= (tcflag_t)~(PARENB | PARODD);
  tio.c_cflag &= (tcflag_t)~CSTOPB;
  tio.c_cflag &= (tcflag_t)~CSIZE;
  tio.c_cflag |= CS8;
  tio.c_cflag &= (tcflag_t)~CRTSCTS;
  tio.c_iflag &= (tcflag_t)~(IXON | IXOFF | IXANY);
  tio.c_cc[VMIN] = 0;
  tio.c_cc[VTIME] = 0;

  if (tcsetattr(fd, TCSANOW, &tio) != 0) {
    std::cerr << "tcsetattr: " << std::strerror(errno) << "\n";
    close(fd);
    return -1;
  }
  tlog(verbose, "termios: raw 8N1, no flow control, VMIN=0 VTIME=0");

  if (sp == 0) {
#if defined(__linux__)
    if (!linux_set_nonstandard_baud(fd, baud)) {
      std::cerr << "Non-standard baud " << baud << " (termios2/BOTHER): "
                << std::strerror(errno) << "\n";
      close(fd);
      return -1;
    }
    tlog(verbose, "baud: Linux termios2 BOTHER set to %u", (unsigned)baud);
#elif defined(__APPLE__)
    if (!macos_set_nonstandard_baud(fd, baud)) {
      std::cerr << "Non-standard baud " << baud << " (IOSSIOSPEED): "
                << std::strerror(errno) << "\n";
      close(fd);
      return -1;
    }
    tlog(verbose, "baud: macOS IOSSIOSPEED set to %u", (unsigned)baud);
#else
    std::cerr << "Unsupported baud rate: " << baud
              << " (no B* constant and no OS support in this build)\n";
    close(fd);
    return -1;
#endif
  }

  int fl = fcntl(fd, F_GETFL, 0);
  if (fl < 0 || fcntl(fd, F_SETFL, fl | O_NONBLOCK) != 0) {
    std::cerr << "fcntl O_NONBLOCK: " << std::strerror(errno) << "\n";
    close(fd);
    return -1;
  }

  /* Many USB-UARTs need DTR/RTS on for the remote side to send data. */
#if defined(TIOCMBIS) && defined(TIOCM_DTR) && defined(TIOCM_RTS)
  {
    int bits = TIOCM_DTR | TIOCM_RTS;
    if (ioctl(fd, TIOCMBIS, &bits) == 0) {
      tlog(verbose, "ioctl TIOCMBIS: DTR+RTS asserted");
    } else {
      tlog(verbose, "ioctl TIOCMBIS (DTR+RTS) failed: %s (continuing)", std::strerror(errno));
    }
  }
#endif
  tlog(verbose, "serial open done, O_NONBLOCK set, read/write ready");
  return fd;
}

void glob_append_unique(std::vector<std::string> *out, const char *pattern) {
  glob_t g;
  memset(&g, 0, sizeof g);
  int gr = glob(pattern, GLOB_NOSORT, nullptr, &g);
  if (gr == 0) {
    for (size_t i = 0; i < g.gl_pathc; i++) {
      std::string p = g.gl_pathv[i];
      if (std::find(out->begin(), out->end(), p) == out->end()) {
        out->push_back(std::move(p));
      }
    }
    globfree(&g);
  }
}

void do_list_ports() {
  std::vector<std::string> paths;
#if defined(__APPLE__)
  glob_append_unique(&paths, "/dev/cu.*");
#else
  glob_append_unique(&paths, "/dev/ttyUSB*");
  glob_append_unique(&paths, "/dev/ttyACM*");
  glob_append_unique(&paths, "/dev/ttyS*");
#endif
  std::sort(paths.begin(), paths.end());
  for (const std::string &p : paths) {
    std::cout << p << "\n";
  }
}

bool line_excluded(const std::string &line,
                   const std::vector<std::regex> &excludes) {
  for (const std::regex &re : excludes) {
    if (std::regex_search(line.begin(), line.end(), re))
      return true;
  }
  return false;
}

std::string strip_trailing_cr(std::string line) {
  if (!line.empty() && line.back() == '\r')
    line.pop_back();
  return line;
}

void mirror_tx_to_stdout(bool local_echo, const void *p, size_t n) {
  if (!local_echo || n == 0) {
    return;
  }
  (void)write_all(STDOUT_FILENO, static_cast<const char *>(p), n);
}

bool write_all(int fd, const char *data, size_t len) {
  while (len > 0) {
    ssize_t w = write(fd, data, len);
    if (w < 0) {
      if (errno == EINTR) {
        continue;
      }
      return false;
    }
    if (w == 0) {
      return false;
    }
    data += static_cast<size_t>(w);
    len -= static_cast<size_t>(w);
  }
  return true;
}

/* picocom 风格: ^A 为前导; ^A+^Q 或 ^A+^X(picocom 默认) 结束; 连按两次 ^A 发字面 0x01 */
static constexpr unsigned char kPicocomPrefix = 0x01;  /* ^A */
static constexpr unsigned char kPicocomQuitQ = 0x11;  /* ^Q; IXON 会吞，已在 try_arm 关 */
static constexpr unsigned char kPicocomQuitX = 0x18;  /* ^X */

bool process_stdin_picocom_style(int serfd, int log_fd, const char *in, size_t n, bool *esc_pending,
                                 bool *quit, bool local_echo) {
  *quit = false;
  if (n == 0) {
    return true;
  }
  std::string for_log;
  for_log.reserve(n);
  for (size_t i = 0; i < n; i++) {
    const unsigned char c = static_cast<unsigned char>(in[i]);
    if (!*esc_pending) {
      if (c == kPicocomPrefix) {
        *esc_pending = true;
        continue;
      }
      {
        const char b = static_cast<char>(c);
        if (!write_all(serfd, &b, 1)) {
          return false;
        }
        mirror_tx_to_stdout(local_echo, &b, 1);
        for_log.push_back(b);
      }
    } else {
      *esc_pending = false;
      if (c == kPicocomQuitQ || c == kPicocomQuitX) {
        *quit = true;
        if (log_fd >= 0 && !for_log.empty()) {
          log_serial_wire(log_fd, for_log.data(), for_log.size());
        }
        return true;
      }
      if (c == kPicocomPrefix) {
        const char lit = 0x01;
        if (!write_all(serfd, &lit, 1)) {
          return false;
        }
        mirror_tx_to_stdout(local_echo, &lit, 1);
        for_log.push_back(lit);
        continue;
      }
      const char prefix = 0x01;
      const char ch = static_cast<char>(c);
      if (!write_all(serfd, &prefix, 1) || !write_all(serfd, &ch, 1)) {
        return false;
      }
      mirror_tx_to_stdout(local_echo, &prefix, 1);
      mirror_tx_to_stdout(local_echo, &ch, 1);
      for_log.push_back(prefix);
      for_log.push_back(ch);
    }
  }
  if (log_fd >= 0 && !for_log.empty()) {
    log_serial_wire(log_fd, for_log.data(), for_log.size());
  }
  return true;
}

void write_filtered_line_to_stdout(const std::string &line,
                                    const std::vector<std::regex> &excludes,
                                    bool *drop_continuation) {
  if (drop_continuation != nullptr && *drop_continuation) {
    *drop_continuation = false;
    return;
  }
  if (line_excluded(line, excludes)) {
    return;
  }
  std::string out = line;
  out.push_back('\n');
  (void)write_all(STDOUT_FILENO, out.data(), out.size());
}

/* Idle partial buffer: must NOT append \\n — that would split ANSI/UTF-8 mid-sequence and cause
 * replacement chars and a messed-up screen (e.g. after "clear"). */
void write_filtered_chunk_to_stdout(const std::string &chunk,
                                   const std::vector<std::regex> &excludes,
                                   bool *drop_continuation) {
  if (chunk.empty()) {
    return;
  }
  std::string s = chunk;
  if (drop_continuation != nullptr && *drop_continuation) {
    const size_t nl = s.find('\n');
    if (nl == std::string::npos) {
      return;
    }
    s.erase(0, nl + 1);
    *drop_continuation = false;
  }
  if (s.empty()) {
    return;
  }
  if (line_excluded(s, excludes)) {
    if (s.find('\n') == std::string::npos) {
      if (drop_continuation != nullptr) {
        *drop_continuation = true;
      }
    }
    return;
  }
  (void)write_all(STDOUT_FILENO, s.data(), s.size());
}

void process_serial_buffer_for_lines(
    std::string *from, const std::vector<std::regex> &excludes, bool *drop_continuation) {
  /* Do NOT use \\r as a line end (only \\n). Many shells (nsh) use \\r alone for cursor/prompt, so
   * splitting on \\r produced fake short “lines” like "cl" and invalid UTF-8 pieces on screen. */
  for (;;) {
    if (from->empty()) {
      return;
    }
    const size_t n = from->find('\n');
    if (n == std::string::npos) {
      return;
    }
    std::string line = from->substr(0, n);
    from->erase(0, n + 1);
    line = strip_trailing_cr(std::move(line));
    write_filtered_line_to_stdout(line, excludes, drop_continuation);
  }
}

void flush_serial_buffer_idle(std::string *from, const std::vector<std::regex> &excludes,
                              bool *drop_continuation) {
  if (from->empty()) {
    return;
  }
  std::string chunk = strip_trailing_cr(*from);
  from->clear();
  write_filtered_chunk_to_stdout(chunk, excludes, drop_continuation);
}

int run_monitor(const Options &opt) {
  tlog(opt.verbose, "start monitor: port=%s baud=%u lecho=%d no_idle_flush=%d excludes=%zu", opt.port.c_str(),
       (unsigned)opt.baud, (int)opt.local_echo, (int)opt.no_idle_flush, opt.exclude_patterns.size());
  std::vector<std::regex> compiled;
  compiled.reserve(opt.exclude_patterns.size());
  for (size_t i = 0; i < opt.exclude_patterns.size(); i++) {
    try {
      compiled.emplace_back(opt.exclude_patterns[i], std::regex::ECMAScript | std::regex::optimize);
    } catch (const std::regex_error &e) {
      std::cerr << "Invalid regex in --exclude (rule " << (i + 1) << "): " << e.what() << "\n";
      return 2;
    }
  }

  int log_fd = -1;
  if (!opt.log_path.empty()) {
    log_fd = open(opt.log_path.c_str(), O_CREAT | O_WRONLY | O_APPEND, 0644);
    if (log_fd < 0) {
      std::cerr << "open --log " << opt.log_path << ": " << std::strerror(errno) << "\n";
      return 1;
    }
  }
  LogFileGuard log_guard{log_fd};
  tlog(opt.verbose, "wire log: %s",
       opt.log_path.empty() ? "(off)" : opt.log_path.c_str());

  int serfd = open_serial_port(opt.port.c_str(), opt.baud, opt.verbose);
  if (serfd < 0) {
    return 1;
  }

  /* Cbreak + ISIG/IXON off: per-key to serial, picocom exit, ^C/Tab as bytes. */
  LocalTermiosGuard local_tty;
  if (!local_tty.try_arm(STDIN_FILENO)) {
    std::cerr << "tcsetattr on stdin failed (expected a TTY, or use pipe/redirection for limited stdin): "
              << std::strerror(errno) << "\n";
    close(serfd);
    return 1;
  }

  std::string from_serial;
  /* If an excluded -e partial was flushed (no \\n) before a USB read boundary, the next bytes
   * are still the same logical line; suppress through the next \\n in the output stream. */
  bool serial_drop_continuation = false;
  std::uint64_t serial_in_total = 0;
  std::uint64_t serial_reads = 0;
  unsigned poll_idle_streak = 0;

  struct pollfd pfds[2];
  /* Poll ignores fd == -1; if stdin is closed/EOF we stop polling it but keep reading serial. */
  pfds[0].fd = STDIN_FILENO;
  pfds[0].events = POLLIN;
  pfds[1].fd = serfd;
  pfds[1].events = POLLIN;

  const int kPollIdleMs = 200;
  tlog(opt.verbose, "mode: cbreak TX; RX %s; stdin is%sa tty", opt.exclude_patterns.empty() ? "pass (no -e) or line+exclude" : "line+exclude",
       isatty(STDIN_FILENO) ? " " : " not ");
  tlog(opt.verbose, "entering poll loop (timeout %dms per tick)", kPollIdleMs);
  tlog(opt.verbose, "  (all lines prefixed with [ttygg] are on stderr; serial text is on stdout)");

  char buf[4096];
  bool picocom_esc_pending = false;

  for (;;) {
    int r = poll(pfds, 2, kPollIdleMs);
    if (r < 0) {
      if (errno == EINTR) {
        tlog(opt.verbose, "poll: EINTR, retrying");
        continue;
      }
      tlog(opt.verbose, "poll: fatal, errno=%d (%s)", errno, std::strerror(errno));
      std::cerr << "poll: " << std::strerror(errno) << "\n";
      break;
    }
    if (r == 0) {
      ++poll_idle_streak;
      if (!opt.no_idle_flush && !opt.exclude_patterns.empty()) {
        flush_serial_buffer_idle(&from_serial, compiled, &serial_drop_continuation);
      }
      if (opt.verbose) {
        if (poll_idle_streak == 1 || poll_idle_streak == 5 ||
            (poll_idle_streak % 25u) == 0u) {
          tlog(true,
               "poll: idle tick #%u (no fd ready; partial_line_buf=%zu bytes; "
               "serial_in_total=%llu bytes, read_calls=%llu)",
               poll_idle_streak, from_serial.size(),
               static_cast<unsigned long long>(serial_in_total),
               static_cast<unsigned long long>(serial_reads));
        }
      }
      continue;
    }
    poll_idle_streak = 0;

    tlog(opt.verbose, "poll: r=%d, stdin revents=0x%x, serial revents=0x%x, stdin_fd=%d", r,
         static_cast<unsigned>(pfds[0].revents & 0xffff),
         static_cast<unsigned>(pfds[1].revents & 0xffff), pfds[0].fd);

    bool want_quit = false;

    /* Drain serial before stdin: shells redraw with \\r/CSI; sending the next key first makes the
     * line look wrong (tab/backspace/completion order). */
    if (pfds[1].revents & (POLLIN | POLLHUP | POLLERR | POLLNVAL)) {
      if (pfds[1].revents & (POLLHUP | POLLERR | POLLNVAL)) {
        if (!(pfds[1].revents & POLLIN)) {
          tlog(opt.verbose,
               "exit loop: serial revents=0x%x (HUP/ERR/NVAL, no POLLIN) — port closed or error",
               static_cast<unsigned>(pfds[1].revents & 0xffff));
          break;
        }
        tlog(opt.verbose, "note: serial has both error bits and POLLIN; will try read");
      }
      for (;;) {
        ssize_t n = read(serfd, buf, sizeof buf);
        if (n < 0) {
          if (errno == EINTR) {
            tlog(opt.verbose, "read serial: EINTR");
            continue;
          }
          if (errno == EAGAIN) {
            break;
          }
          tlog(opt.verbose, "read serial: fatal errno=%d %s", errno, std::strerror(errno));
          std::cerr << "read serial: " << std::strerror(errno) << "\n";
          close(serfd);
          return 1;
        }
        if (n == 0) {
          tlog(opt.verbose, "read serial: returned 0 (driver EOF?)");
          break;
        }
        ++serial_reads;
        serial_in_total += static_cast<std::uint64_t>(n);
        if (log_fd >= 0) {
          log_serial_wire(log_fd, buf, static_cast<size_t>(n));
        }
        tlog_serial_read_hex(opt.verbose, reinterpret_cast<const uint8_t *>(buf),
                             static_cast<size_t>(n));
        tlog(opt.verbose, "read serial: +%zd bytes (total in=%llu)", n,
             static_cast<unsigned long long>(serial_in_total));
        if (opt.exclude_patterns.empty()) {
          if (!write_all(STDOUT_FILENO, buf, static_cast<size_t>(n))) {
            tlog(opt.verbose, "write stdout failed");
            std::cerr << "write stdout: " << std::strerror(errno) << "\n";
            close(serfd);
            return 1;
          }
        } else {
          from_serial.append(buf, static_cast<size_t>(n));
          process_serial_buffer_for_lines(&from_serial, compiled, &serial_drop_continuation);
        }
      }
      /* With -e, line editing (backspace / \\r redraw) has no \\n; waiting only for poll idle (~200ms)
       * made deletes very sluggish. Flush partial after each drain; same risk as idle (UTF-8/ANSI split);
       * use --no-idle-flush to disable both. */
      if (!opt.exclude_patterns.empty() && !opt.no_idle_flush) {
        flush_serial_buffer_idle(&from_serial, compiled, &serial_drop_continuation);
      }
    }

    if (pfds[0].fd >= 0 && (pfds[0].revents & (POLLIN | POLLHUP | POLLERR))) {
      ssize_t n = read(pfds[0].fd, buf, sizeof buf);
      if (n < 0) {
        if (errno == EINTR) {
          tlog(opt.verbose, "read stdin: EINTR");
        } else if (errno == EAGAIN) {
          tlog(opt.verbose, "read stdin: EAGAIN");
        } else {
          tlog(opt.verbose, "read stdin: errno=%d %s", errno, std::strerror(errno));
          std::cerr << "read stdin: " << std::strerror(errno) << "\n";
        }
      } else if (n == 0) {
        tlog(opt.verbose, "read stdin: EOF, stop polling stdin (serial continues)");
        pfds[0].fd = -1; /* EOF: stop watching stdin, do not exit */
      } else {
        tlog(opt.verbose, "read stdin: %zd bytes, picocom-style to serial", n);
        if (!process_stdin_picocom_style(serfd, log_fd, buf, static_cast<size_t>(n), &picocom_esc_pending,
                                         &want_quit, opt.local_echo)) {
          tlog(opt.verbose, "write stdin to serial failed");
          std::cerr << "write serial: " << std::strerror(errno) << "\n";
          close(serfd);
          return 1;
        }
      }
    }

    if (want_quit) {
      tlog(opt.verbose, "exit: picocom-style ^A then ^Q or ^X");
      std::cerr << "ttygg: exit (Ctrl+A, then Ctrl+Q or Ctrl+X)\n";
      break;
    }
  }

  tlog(opt.verbose, "monitor end, serial_in_total=%llu bytes, read() calls=%llu",
       static_cast<unsigned long long>(serial_in_total),
       static_cast<unsigned long long>(serial_reads));
  close(serfd);
  return 0;
}

} // namespace

int main(int argc, char **argv) {
  Options opt;
  std::string err;
  if (!parse_args(argc, argv, &opt, &err)) {
    std::cerr << err << "\n";
    print_help(argv[0]);
    return 2;
  }
  if (opt.help) {
    print_help(argv[0]);
    return 0;
  }
  if (opt.list_ports) {
    do_list_ports();
    return 0;
  }
  {
    const char *dbg = std::getenv("TTYGG_DEBUG");
    if (dbg != nullptr && dbg[0] != '\0' && std::strcmp(dbg, "0") != 0) {
      opt.verbose = true;
    }
  }
  {
    const char *ni = std::getenv("TTYGG_NO_IDLE");
    if (ni != nullptr && std::strcmp(ni, "0") != 0) {
      opt.no_idle_flush = true;
    }
  }
  if (opt.verbose) {
    std::cerr << "[ttygg] verbose on (-v, --verbose, or TTYGG_DEBUG=1), traces go to stderr\n";
  }
  return run_monitor(opt);
}
