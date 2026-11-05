# Pi Serial Logger

[![CI](https://github.com/HannaBeshoy/pi-serial-logger/actions/workflows/ci.yml/badge.svg)](https://github.com/HannaBeshoy/pi-serial-logger/actions/workflows/ci.yml)

**System Programming course project** · University of Debrecen, Faculty of Informatics · C (POSIX), Linux, Raspberry Pi · **tested on Linux with a simulated serial port; Raspberry Pi test on the December lab day**

The Raspberry Pi end of the sensor chain: a C program that reads the Arduino sensor node over USB serial, checks every frame, logs good readings to daily CSV files, and serves the latest values to the network. It is built from the system calls of the course: command-line arguments and return values, environment variables, time handling, termios, `fork` and pipes, signals, low-level file I/O sized by the inode's block size, and TCP and UDP sockets with `select`.

- [x] `logger`: serial reader (termios raw mode, 115200 baud), XOR-checksum check, `fork`ed writer process fed through a pipe
- [x] TCP server (LAST, STATS, STREAM, QUIT) and UDP server (LAST, STATS) in one `select` loop
- [x] Signals: SIGINT/SIGTERM clean shutdown, SIGHUP log reopen for rotation, SIGCHLD restarts a dead writer, all through a self-pipe
- [x] `fake_node`: an Arduino simulated on a pseudo-terminal, with corrupted frames and noise
- [x] The course's complex practicing task (`coder`: bit-swap coding with a temporary file and an st_blksize buffer)
- [x] **13 unit checks** and **8 integration tests** (every good frame logged, every corrupted one caught, rotation, a killed writer replaced without a zombie, clean shutdown)
- [x] systemd unit for the Pi
- [ ] Lab day: on the Raspberry Pi with the real Arduino ([test sheet](docs/LAB_DAY.md))

## How it works

```text
 Arduino ──USB serial──► logger (parent) ──pipe──► writer (child, fork)
 $SN,42,...*63           termios raw 115200          appends to DIR/2026-11-05.csv
                         checks the checksum         buffer = st_blksize of the file
                         select() on: serial, TCP listener, TCP clients, UDP, self-pipe
                         ▲          ▲
          nc pi 5600 ────┘          └──── UDP "LAST" datagram on port 5601
          LAST / STATS / STREAM            → one datagram back
```

- **Two processes.** The parent must never stall reading the serial port; disk writes can. So the parent hands each line to a child through a pipe. Lines are shorter than `PIPE_BUF`, so each `write` to the pipe is atomic.
- **One loop, no threads.** `select` waits on the serial port, the listening TCP socket, every connected client, the UDP socket and the self-pipe at once.
- **Signals done safely.** A handler may only call async-signal-safe functions, so each handler just writes the signal number into a pipe; the main loop reads it and does the work (forward SIGHUP to the child, `waitpid` the dead child, shut down).
- **Clean shutdown.** On SIGINT or SIGTERM the parent closes the pipe; the child sees end of file, writes its buffer and exits 0; the parent waits for it. If the child is killed, SIGCHLD makes the parent reap it (no zombie) and start a new one.
- **Checked input.** A frame is accepted only with a `$`, a `*`, two hex digits that match the XOR of the body, and every field in range. The unit test flips every single bit of a frame and checks that each one is caught.

## Interfaces

| Interface | Example |
| --- | --- |
| log files | `~/sensor-logs/2026-11-05.csv`: `time,seq,uptime_ms,temp_c,dist_cm,light_pct` |
| TCP 5600 | `printf 'LAST\n' \| nc pi 5600` → `2026-11-05T21:40:00,42,123456,29.8,34.3,50` |
| TCP 5600 | `STATS`, or `STREAM` to receive every new line |
| UDP 5601 | `echo -n LAST \| nc -u -w1 pi 5601` |
| signals | `kill -HUP <pid>` reopens the log; `kill <pid>` or Ctrl-C stops it cleanly |

## Running it

```bash
make && make test
./build/fake_node -n 1000 -t 100 &          # prints a device such as /dev/pts/3
./build/logger -d /dev/pts/3 -o /tmp/logs -v
printf 'STREAM\n' | nc localhost 5600
```

On the Raspberry Pi: `sudo apt install build-essential`, `make`, then `./build/logger -d /dev/ttyACM0` (Uno) or `/dev/ttyUSB0` (Nano clones); add the user to the `dialout` group for access to the port. `deploy/sensor-logger.service` runs it at boot.

## The course exercise

`exercises/coder/` is the course's complex practicing task: `coder -c` lists the regular files in `$HOME`, asks for one (again and again until it exists), swaps the first two and last two bits of every byte and inserts a random letter after each, through a temporary file renamed over the original, with a buffer of the file's `st_blksize`, ignoring Ctrl-C for the first 10 seconds; `coder -d` undoes it. The tests code and decode a file with all 256 byte values and check the original comes back.

## Repository layout

```text
src/          logger.c (parent: serial, sockets, signals), writer.c (child: files), frame.c (protocol)
tools/        fake_node.c: the Arduino on a pseudo-terminal
tests/        unit tests (C) and integration tests (Python)
exercises/    the course's coder task
deploy/       systemd unit
docs/         project guide, lab-day test sheet
```

## Author

Beshoy Hanna · Computer Science Engineering BSc, University of Debrecen · [LinkedIn](https://www.linkedin.com/in/beshoy-hanna-8b52213a0)
