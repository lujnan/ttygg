# ttygg

**ttygg** is a small command-line serial monitor for **macOS** and **Linux**. Its main reason to exist is **multi-pattern output filtering**: you can stack many **`-e` / `--exclude`** rules (ECMAScript-style regex). Any line that matches **any** of those patterns is dropped from stdout, so you can peel away different kinds of noise at once—debug spam, periodic heartbeats, vendor banners—while everything else still streams through. Without `-e`, received bytes pass through unfiltered; with `-e`, RX is handled **line-based** (newline-delimited) so filtering stays predictable.

Under the hood it uses `poll(2)` and `termios` for non-blocking I/O and supports configurable baud rates (including non-standard rates via OS-specific ioctls where available).

Interactive mode uses cbreak stdin: each key is forwarded to the serial port (similar in spirit to picocom-style workflows). Serial data is written to stdout; debug traces go to stderr so they do not mix with the data stream.

## Requirements

- **CMake** 3.10 or newer
- A **C++11** compiler
- **macOS**: links against **IOKit** (for serial port discovery / listing)
- **Linux**: standard development headers (`termios`, etc.)

**Note:** The **Linux** port has **not** been run or validated on real hardware or in production-like setups yet; treat it as **untested in the field** until you verify it on your machines.

## Build

From the repository root:

```bash
cmake -S . -B build
cmake --build build
```

The binary is `build/ttygg`. You can install it manually, for example:

```bash
cp build/ttygg /usr/local/bin/   # optional
```

## Quick start

List candidate serial devices:

```bash
./build/ttygg --list
```

Open a port (on macOS, prefer **`/dev/cu.*`** call-out devices; **`/dev/tty.*`** can block on `open(2)`):

```bash
./build/ttygg -p /dev/cu.usbserial-0 -b 115200
```

Stack several exclude rules; a line is hidden if it matches **any** pattern:

```bash
./build/ttygg -p /dev/ttyUSB0 -b 921600 \
  -e '^\[DBG\]' \
  -e '^heartbeat' \
  -e 'SPIFF.*deprecated' \
  -L session.log
```

With `-e`, `session.log` gets **filtered** RX by default (same as the screen). To record **full** RX in the file while still filtering the terminal, add **`--log-full-rx`**:

```bash
./build/ttygg -p /dev/ttyUSB0 -b 921600 -e '^\[DBG\]' --log-full-rx -L session.log
```

Full options and behaviour notes:

```bash
./build/ttygg --help
```

## Project-local config (`.ttygg.conf` or `-c`)

The **ttygg binary** does not need to live next to your config: resolution uses your **shell current working directory** (where you run the command), not the directory that holds the executable.

**Two ways to load a config file:**

1. **Implicit:** If you do **not** pass **`-c` / `--config`**, ttygg looks for a file named **`.ttygg.conf`** in the **current working directory** only (again: not relative to the installed binary). If it exists as a regular file, **`key=value`** lines are loaded (`#` starts comments; keys such as `port` / `device`, `baud`, **`exclude_line`** (repeatable), `log`, `log_dir`; `exclude` / `pattern` are accepted as aliases for `exclude_line`).
2. **Explicit:** **`ttygg --config /path/to/file`** or **`ttygg -c /path/to/file`**. Use any path and filename you like. If **`--config`** is present, ttygg **does not** auto-load `./.ttygg.conf` (only the file you specify). Multiple **`-c`** / **`--config`** arguments can appear; the **last** one wins.

After loading a file (implicit or explicit), **command-line flags still override** the same settings. **`--help`** and **`--list`** do **not** load any config file so broken files cannot block those commands.

Typical implicit workflow: create **`.ttygg.conf`** in a firmware repo, **`cd`** into that repo, run **`ttygg`** from **`PATH`** (no `-p`):

```bash
cd ~/proj/my-board
ttygg   # reads ./.ttygg.conf here
```

Load a fixed profile from elsewhere:

```bash
ttygg --config ~/proj/my-board/ttygg-board.conf
```

Example config (save as **`.ttygg.conf`** next to your project or pass via **`--config`**):

```text
# Rename to ".ttygg.conf" — loaded from the current working directory when you run ttygg
# unless you pass --help/--list or already pass -p on the command line.
# Lines are key=value. Leading/trailing spaces are trimmed. Lines starting with # are comments.
port = /dev/cu.usbserial-1410
baud = 921600
# Log directory: creates/opens <log_dir>/ttygg.log (same as -L that path).
# If you prefer an explicit log file path, use log= instead (and omit log_dir).
log_dir = ./logs
exclude_line = ^\s*heartbeat
exclude_line = SPIFF.*
# Values may be wrapped in "double" or 'single' quotes; those outer quotes are removed and do not
# appear in the regex (e.g. exclude_line="hifi4" matches the substring hifi4, not quote characters).
# Same boolean keys as CLI (optional):
# verbose = true
# local_echo = false
# no_idle_flush = false
# log_full_rx = false
```

## Logging

Use **`-L` / `--log FILE`** to append traffic to a file as **escaped text** (non-printables escaped, newline-terminated records). **Keyboard TX** (what you type toward the serial port) is always logged in full. **Serial RX** in the file depends on filtering:

| Mode | RX in log file |
|------|----------------|
| No `-e` | Same as stdout: raw RX stream. |
| With `-e` (default) | Matches the **terminal**: only lines/chunks that are **not** dropped by `--exclude` are written to the log. |
| With `-e` and `--log-full-rx` | Full **RX wire** in the file; `--exclude` applies only to stdout, not to logged RX. |

**Without `-L`:** no log file is created until you use the hotkey below. After the first **Ctrl+A** then **Ctrl+L**, logging uses an auto-generated name in the current directory.

**With `-L path`:** logging starts immediately, appending to `path`. Each **Ctrl+A** then **Ctrl+L** closes the current log and opens a **new** file. New files are named:

`{stem}_{n}_{YYYYMMDDHHMMSS}.log`

- **`n`** starts from 0 and increases each time; if that name already exists, `n` is bumped until `open(2)` succeeds (`O_EXCL`).
- **`YYYYMMDDHHMMSS`** is local time (14 digits).
- **Without `-L`:** `stem` is **`ttygg`** (working directory).
- **With `-L`:** directory and `stem` come from `path` (e.g. `logs/capture.log` → `logs/capture_0_20260427143022.log`). A trailing `.log` on the basename is stripped for `stem`.

You can rotate the log multiple times in one session; each **Ctrl+A**, **Ctrl+L** starts a fresh file.

## Exiting and log hotkeys

ttygg uses a **Ctrl+A prefix** (picocom-style). The prefix alone is **not** sent to the serial port.

| After Ctrl+A | Action |
|--------------|--------|
| **Ctrl+Q** or **Ctrl+X** | Quit ttygg. |
| **Ctrl+L** | Start a new log file (see [Logging](#logging)). |

To send a **literal byte 0x01** to the device (e.g. some firmware uses Ctrl+A), press **Ctrl+A** twice in a row: the first is the prefix, the second is forwarded as `0x01`.

## Environment variables

| Variable | Effect |
|----------|--------|
| `TTYGG_DEBUG=1` | Same as `-v` / `--verbose` (trace to stderr) |
| `TTYGG_NO_IDLE=1` | Same as `--no-idle-flush` when using line filtering (`-e`) |

## Use and redistribution

This project is offered **without license formalities**: you may **use, copy, modify, and redistribute** it **for any purpose**, with **no warranty** and **no conditions** imposed by the authors.
