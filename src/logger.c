/*
 * logger.c - Raspberry Pi sensor logger (System Programming course project).
 *
 *   logger -d /dev/ttyACM0 [-b 115200] [-o DIR] [-p 5600] [-u 5601] [-v]
 *
 * The parent process reads the Arduino's lines from the serial port, checks each
 * frame's XOR checksum, and passes good readings through a pipe to a child process
 * (created with fork) that writes them to a daily CSV file. The parent also answers
 * clients: over TCP (connection-oriented: LAST, STATS, STREAM, QUIT) and over UDP
 * (connectionless: a LAST or STATS datagram gets one datagram back).
 *
 * Signals: SIGINT / SIGTERM shut down cleanly (the pipe is closed, the child writes
 * what is left and exits, the parent waits for it); SIGHUP makes the child reopen its
 * file (for log rotation); if the child dies, the parent starts a new one.
 * Signal handlers only write one byte into a "self-pipe"; the main loop's select()
 * sees it and does the real work outside the handler.
 *
 * The log directory is -o, else $SENSOR_LOG_DIR, else $HOME/sensor-logs.
 */
#define _DEFAULT_SOURCE
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>
#include "frame.h"
#include "writer.h"

#define MAX_CLIENTS 16

typedef struct { int fd; bool stream; char in[128]; size_t in_len; } client_t;

static int sig_pipe[2];                                 /* the self-pipe */
static void on_signal(int sig) { int e = errno; unsigned char s = (unsigned char)sig; (void)!write(sig_pipe[1], &s, 1); errno = e; }

static struct {
    unsigned long lines, good, bad[6], dropped_clients, child_restarts;
    char last[160];
    time_t started;
} st;

static speed_t baud_constant(long b)
{
    switch (b) {
    case 9600: return B9600; case 19200: return B19200; case 38400: return B38400;
    case 57600: return B57600; case 115200: return B115200; default: return 0;
    }
}

static int open_serial(const char *path, long baud)
{
    if (strcmp(path, "-") == 0) return STDIN_FILENO;
    int fd = open(path, O_RDWR | O_NOCTTY);
    if (fd < 0) { perror(path); return -1; }
    if (isatty(fd)) {                                  /* a pipe or a plain file needs no settings */
        struct termios t;
        if (tcgetattr(fd, &t) < 0) { perror("tcgetattr"); close(fd); return -1; }
        cfmakeraw(&t);                                 /* 8 bits, no echo, no line editing */
        t.c_cflag |= CLOCAL | CREAD;
        t.c_cc[VMIN] = 1; t.c_cc[VTIME] = 0;
        speed_t sp = baud_constant(baud);
        if (!sp) { fprintf(stderr, "unsupported baud rate %ld\n", baud); close(fd); return -1; }
        cfsetispeed(&t, sp); cfsetospeed(&t, sp);
        if (tcsetattr(fd, TCSANOW, &t) < 0) { perror("tcsetattr"); close(fd); return -1; }
        tcflush(fd, TCIFLUSH);                         /* drop what arrived before we started */
    }
    return fd;
}

static int listen_tcp(int port)
{
    int s = socket(AF_INET, SOCK_STREAM, 0), one = 1;
    setsockopt(s, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
    struct sockaddr_in a = { .sin_family = AF_INET, .sin_port = htons(port), .sin_addr.s_addr = htonl(INADDR_ANY) };
    if (bind(s, (struct sockaddr *)&a, sizeof a) < 0 || listen(s, 8) < 0) { perror("tcp"); close(s); return -1; }
    return s;
}

static int bind_udp(int port)
{
    int s = socket(AF_INET, SOCK_DGRAM, 0);
    struct sockaddr_in a = { .sin_family = AF_INET, .sin_port = htons(port), .sin_addr.s_addr = htonl(INADDR_ANY) };
    if (bind(s, (struct sockaddr *)&a, sizeof a) < 0) { perror("udp"); close(s); return -1; }
    return s;
}

static pid_t start_writer(int *pipe_w, const char *dir, int keep_fd[], int nkeep)
{
    int p[2];
    if (pipe(p) < 0) { perror("pipe"); return -1; }
    pid_t pid = fork();
    if (pid < 0) { perror("fork"); return -1; }
    if (pid == 0) {                                    /* child: only the read end of the pipe */
        close(p[1]);
        for (int i = 0; i < nkeep; i++) if (keep_fd[i] >= 0) close(keep_fd[i]);
        signal(SIGINT, SIG_IGN);                       /* Ctrl-C goes to the whole group: the parent decides */
        signal(SIGTERM, SIG_DFL);
        _exit(writer_main(p[0], dir));
    }
    close(p[0]);
    *pipe_w = p[1];
    return pid;
}

static void stats_text(char *buf, size_t cap)
{
    snprintf(buf, cap, "uptime %lds lines %lu good %lu bad_checksum %lu bad_fields %lu no_start %lu no_star %lu bad_hex %lu child_restarts %lu\n",
             (long)(time(NULL) - st.started), st.lines, st.good, st.bad[FRAME_BAD_CHECKSUM], st.bad[FRAME_BAD_FIELDS],
             st.bad[FRAME_NO_START], st.bad[FRAME_NO_STAR], st.bad[FRAME_BAD_HEX], st.child_restarts);
}

static bool send_all(int fd, const char *s, size_t n)
{
    while (n) {
        ssize_t k = send(fd, s, n, MSG_NOSIGNAL);
        if (k <= 0) return false;
        s += k; n -= (size_t)k;
    }
    return true;
}

static void drop_client(client_t *c) { close(c->fd); c->fd = -1; st.dropped_clients++; }

/* one complete line from the serial port */
static void handle_line(char *line, int pipe_w, client_t *cl)
{
    st.lines++;
    frame_t f;
    frame_error_t e = frame_parse(line, &f);
    if (e != FRAME_OK) { st.bad[e]++; return; }
    st.good++;
    time_t now = time(NULL);
    struct tm tm;
    localtime_r(&now, &tm);
    char ts[32], csv[160];
    strftime(ts, sizeof ts, "%Y-%m-%dT%H:%M:%S", &tm);
    double d = frame_dist_cm(&f);
    int n = d < 0 ? snprintf(csv, sizeof csv, "%s,%lu,%lu,%.1f,,%d\n", ts, (unsigned long)f.seq, (unsigned long)f.uptime_ms, frame_temp_c(&f), f.light_adc * 100 / 1023)
                  : snprintf(csv, sizeof csv, "%s,%lu,%lu,%.1f,%.1f,%d\n", ts, (unsigned long)f.seq, (unsigned long)f.uptime_ms, frame_temp_c(&f), d, f.light_adc * 100 / 1023);
    memcpy(st.last, csv, (size_t)n + 1);
    if (pipe_w >= 0 && write(pipe_w, csv, (size_t)n) != n)   /* lines < PIPE_BUF are written atomically */
        fprintf(stderr, "logger: pipe write failed: %s\n", strerror(errno));
    for (int i = 0; i < MAX_CLIENTS; i++)
        if (cl[i].fd >= 0 && cl[i].stream && !send_all(cl[i].fd, csv, (size_t)n)) drop_client(&cl[i]);
}

static void handle_command(client_t *c, const char *cmd)
{
    char out[256];
    if (strcmp(cmd, "LAST") == 0) { if (!send_all(c->fd, st.last[0] ? st.last : "none\n", strlen(st.last[0] ? st.last : "none\n"))) drop_client(c); }
    else if (strcmp(cmd, "STATS") == 0) { stats_text(out, sizeof out); if (!send_all(c->fd, out, strlen(out))) drop_client(c); }
    else if (strcmp(cmd, "STREAM") == 0) c->stream = true;
    else if (strcmp(cmd, "QUIT") == 0) drop_client(c);
    else if (!send_all(c->fd, "ERR commands: LAST STATS STREAM QUIT\n", 37)) drop_client(c);
}

int main(int argc, char **argv)
{
    const char *dev = NULL, *dir = getenv("SENSOR_LOG_DIR");
    long baud = 115200;
    int tcp_port = 5600, udp_port = 5601, verbose = 0, opt;
    char home_dir[512];
    while ((opt = getopt(argc, argv, "d:b:o:p:u:v")) != -1) {
        switch (opt) {
        case 'd': dev = optarg; break;
        case 'b': baud = strtol(optarg, NULL, 10); break;
        case 'o': dir = optarg; break;
        case 'p': tcp_port = atoi(optarg); break;
        case 'u': udp_port = atoi(optarg); break;
        case 'v': verbose = 1; break;
        default: goto usage;
        }
    }
    if (!dev) {
    usage:
        fprintf(stderr, "usage: %s -d DEVICE|- [-b BAUD] [-o DIR] [-p TCP_PORT] [-u UDP_PORT] [-v]\n", argv[0]);
        return 1;
    }
    if (!dir) {
        const char *home = getenv("HOME");
        snprintf(home_dir, sizeof home_dir, "%s/sensor-logs", home ? home : ".");
        dir = home_dir;
    }
    if (mkdir(dir, 0755) < 0 && errno != EEXIST) { perror(dir); return 1; }

    int serial = open_serial(dev, baud);
    int tcp = listen_tcp(tcp_port), udp = bind_udp(udp_port);
    if (serial < 0 || tcp < 0 || udp < 0) return 1;
    if (pipe(sig_pipe) < 0) { perror("pipe"); return 1; }
    fcntl(sig_pipe[0], F_SETFL, O_NONBLOCK);           /* both ends: the handler never blocks, the loop drains it */
    fcntl(sig_pipe[1], F_SETFL, O_NONBLOCK);
    struct sigaction sa = { .sa_handler = on_signal };
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = SA_RESTART;
    sigaction(SIGINT, &sa, NULL); sigaction(SIGTERM, &sa, NULL);
    sigaction(SIGHUP, &sa, NULL); sigaction(SIGCHLD, &sa, NULL);
    signal(SIGPIPE, SIG_IGN);

    client_t cl[MAX_CLIENTS];
    for (int i = 0; i < MAX_CLIENTS; i++) cl[i].fd = -1;
    int pipe_w = -1;
    int keep[] = { serial == STDIN_FILENO ? -1 : serial, tcp, udp, sig_pipe[0], sig_pipe[1] };
    pid_t child = start_writer(&pipe_w, dir, keep, 5);
    if (child < 0) return 1;
    st.started = time(NULL);
    fprintf(stderr, "logger: pid %d, writer pid %d, reading %s, logging to %s, tcp %d, udp %d\n",
            (int)getpid(), (int)child, dev, dir, tcp_port, udp_port);

    char line[256];
    size_t len = 0;
    bool running = true, serial_open = true;
    while (running) {
        fd_set rd;
        FD_ZERO(&rd);
        int maxfd = sig_pipe[0];
        FD_SET(sig_pipe[0], &rd);
        if (serial_open) { FD_SET(serial, &rd); if (serial > maxfd) maxfd = serial; }
        FD_SET(tcp, &rd); if (tcp > maxfd) maxfd = tcp;
        FD_SET(udp, &rd); if (udp > maxfd) maxfd = udp;
        for (int i = 0; i < MAX_CLIENTS; i++) if (cl[i].fd >= 0) { FD_SET(cl[i].fd, &rd); if (cl[i].fd > maxfd) maxfd = cl[i].fd; }
        if (select(maxfd + 1, &rd, NULL, NULL, NULL) < 0) { if (errno == EINTR) continue; perror("select"); break; }

        if (FD_ISSET(sig_pipe[0], &rd)) {
            unsigned char s;
            while (read(sig_pipe[0], &s, 1) == 1) {
                if (s == SIGINT || s == SIGTERM) running = false;
                else if (s == SIGHUP) { if (child > 0) kill(child, SIGHUP); }
                else if (s == SIGCHLD) {
                    int status;
                    pid_t p;
                    while ((p = waitpid(-1, &status, WNOHANG)) > 0)       /* reap it: no zombie */
                        if (p == child && running) {
                            fprintf(stderr, "logger: writer %d stopped (status %d), starting a new one\n", (int)p, status);
                            close(pipe_w);
                            child = start_writer(&pipe_w, dir, keep, 5);
                            st.child_restarts++;
                        }
                }
            }
        }
        if (serial_open && FD_ISSET(serial, &rd)) {
            char buf[512];
            ssize_t n = read(serial, buf, sizeof buf);
            if (n <= 0) {
                if (n < 0 && errno == EINTR) continue;
                fprintf(stderr, "logger: %s closed\n", dev);
                serial_open = false;
                if (strcmp(dev, "-") == 0 || !isatty(serial)) running = false;   /* end of input: done */
            }
            for (ssize_t i = 0; i < n; i++) {
                if (buf[i] == '\n' || buf[i] == '\r') {
                    if (len) { line[len] = '\0'; handle_line(line, pipe_w, cl); if (verbose) fprintf(stderr, "%s\n", line); }
                    len = 0;
                } else if (len < sizeof line - 1) line[len++] = buf[i];
                else len = 0;                                             /* overlong line: discard */
            }
        }
        if (FD_ISSET(tcp, &rd)) {
            int fd = accept(tcp, NULL, NULL);
            int slot = -1;
            for (int i = 0; i < MAX_CLIENTS && slot < 0; i++) if (cl[i].fd < 0) slot = i;
            if (fd >= 0 && slot < 0) { send_all(fd, "ERR busy\n", 9); close(fd); }
            else if (fd >= 0) { cl[slot].fd = fd; cl[slot].stream = false; cl[slot].in_len = 0; }
        }
        if (FD_ISSET(udp, &rd)) {
            char req[64], out[256];
            struct sockaddr_in from;
            socklen_t fl = sizeof from;
            ssize_t n = recvfrom(udp, req, sizeof req - 1, 0, (struct sockaddr *)&from, &fl);
            if (n > 0) {
                req[n] = '\0';
                req[strcspn(req, "\r\n")] = '\0';
                if (strcmp(req, "STATS") == 0) stats_text(out, sizeof out);
                else if (strcmp(req, "LAST") == 0) snprintf(out, sizeof out, "%s", st.last[0] ? st.last : "none\n");
                else snprintf(out, sizeof out, "ERR datagrams: LAST STATS\n");
                sendto(udp, out, strlen(out), 0, (struct sockaddr *)&from, fl);
            }
        }
        for (int i = 0; i < MAX_CLIENTS; i++) {
            client_t *c = &cl[i];
            if (c->fd < 0 || !FD_ISSET(c->fd, &rd)) continue;
            ssize_t n = recv(c->fd, c->in + c->in_len, sizeof c->in - 1 - c->in_len, 0);
            if (n <= 0) { drop_client(c); continue; }
            c->in_len += (size_t)n;
            char *nl;
            while (c->fd >= 0 && (nl = memchr(c->in, '\n', c->in_len))) {
                *nl = '\0';
                if (nl > c->in && nl[-1] == '\r') nl[-1] = '\0';
                handle_command(c, c->in);
                if (c->fd < 0) break;
                size_t used = (size_t)(nl - c->in) + 1;
                memmove(c->in, nl + 1, c->in_len - used);
                c->in_len -= used;
            }
            if (c->fd >= 0 && c->in_len == sizeof c->in - 1) drop_client(c);   /* a "line" too long */
        }
    }

    /* shutdown: closing the pipe tells the child to finish; then wait for it */
    close(pipe_w);
    int status = 0;
    signal(SIGCHLD, SIG_DFL);
    if (child > 0) waitpid(child, &status, 0);
    for (int i = 0; i < MAX_CLIENTS; i++) if (cl[i].fd >= 0) close(cl[i].fd);
    char s[256];
    stats_text(s, sizeof s);
    fprintf(stderr, "logger: stopped; writer exit status %d; %s", WIFEXITED(status) ? WEXITSTATUS(status) : -1, s);
    return 0;
}
