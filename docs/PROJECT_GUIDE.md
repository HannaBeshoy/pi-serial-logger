# Project guide

Each topic of the System Programming notes (SP_note.pdf and the socket notes) and where the logger uses it.

| Topic in the notes | In this project |
| --- | --- |
| Command-line arguments, return value | `getopt` options; exit code 1 on a usage error, 0 after a clean shutdown |
| Environment variables | `SENSOR_LOG_DIR`, else `HOME` |
| Time handling | `time`, `localtime_r`, `strftime` for timestamps and the daily file name |
| Multi-source programs, header files | `frame.h`, `writer.h`; the exercise's `secret.h` |
| Pointers, dynamic memory | the writer's buffer is `malloc`ed at the file's block size |
| Bitwise operations | the XOR checksum; the coder's bit swap |
| Low-level file handling | `open`, `read`, `write`, `close`, `O_APPEND`; `fstat` for `st_blksize` |
| Directories and inodes | the coder lists `$HOME` with `opendir`/`readdir` and `lstat` (regular files only) |
| Processes and fork | the writer child; `waitpid` reaps it |
| Signals | SIGINT, SIGTERM, SIGHUP, SIGCHLD, SIGPIPE; the coder ignores SIGINT for 10 s with `alarm` |
| Sockets, connectionless | UDP: one datagram in, one out, no connection state |
| Sockets, connection-oriented | TCP: `listen`, `accept`, per-client buffers, line-based commands |

## 1. The serial port

A USB serial adapter appears as a tty. By default the tty driver edits lines, echoes characters and translates line endings, which would corrupt binary data and delay input. `cfmakeraw` turns all of that off; `VMIN = 1, VTIME = 0` makes `read` return as soon as one byte is there. The logger builds lines itself, accepting `\r\n` or `\n`.

## 2. Why the checksum matters

USB serial is reliable on a short cable, but the Arduino resets when the port opens and prints boot noise, and a long or noisy line can flip bits. The XOR of the body detects every single-bit error (the test flips each of about 200 bits in turn). It does not detect two errors in the same bit position, which a CRC would; for this link the trade-off is fine and the Arduino computes it in one instruction per byte.

## 3. The self-pipe trick

A signal can arrive between any two instructions, so a handler may call only async-signal-safe functions (`write` is one; `printf`, `malloc`, `waitpid` with side effects on shared state are not safe there). The handler writes the signal number into a non-blocking pipe; `select` wakes up because that pipe is readable, and the main loop handles the signal at a safe point. This turns asynchronous signals into ordinary events.

## 4. Zombies

A child that exits stays in the process table as a zombie until its parent calls `wait`. On SIGCHLD the logger calls `waitpid(-1, ..., WNOHANG)` in a loop (several children may have exited for one signal), and the test kills the writer and checks with `ps` that no zombie remains.

## Notes for the three tracks

- **Embedded systems and edge AI.** A gateway like this collects data from microcontrollers and feeds it on; on the Pi it is the place to run a small model on the stream.
- **FPGA to semiconductors.** The same framing and checksum ideas protect data on any serial hardware link; an FPGA UART receiver would check the frame in logic before handing it on.
- **Product security.** Input from the port is untrusted: bounded line length, strict parsing, checksum, range checks. Network clients get line-length limits and a cap of 16 connections; `MSG_NOSIGNAL` and ignoring SIGPIPE mean a client that disappears cannot kill the logger.
