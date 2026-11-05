/*
 * writer.c - the logger's child process. Low-level I/O only (open, read, write):
 * the output buffer is as large as the file system's preferred block size
 * (st_blksize from fstat), so each write() fills whole blocks. A new file starts
 * at midnight, and SIGHUP closes and reopens the file (the usual way to rotate logs).
 */
#define _DEFAULT_SOURCE
#include "writer.h"
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/select.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

static volatile sig_atomic_t reopen_requested = 0;
static void on_hup(int sig) { (void)sig; reopen_requested = 1; }

static const char HEADER[] = "time,seq,uptime_ms,temp_c,dist_cm,light_pct\n";

static int open_day(const char *dir, char *day, size_t daylen)
{
    time_t now = time(NULL);
    struct tm tm;
    localtime_r(&now, &tm);
    strftime(day, daylen, "%Y-%m-%d", &tm);
    char path[600];
    snprintf(path, sizeof path, "%s/%s.csv", dir, day);
    int fd = open(path, O_WRONLY | O_CREAT | O_APPEND, 0644);
    if (fd < 0) { fprintf(stderr, "writer: %s: %s\n", path, strerror(errno)); return -1; }
    struct stat sb;
    if (fstat(fd, &sb) == 0 && sb.st_size == 0 && write(fd, HEADER, sizeof HEADER - 1) < 0) { close(fd); return -1; }
    return fd;
}

static int flush(int fd, char *buf, size_t *n)
{
    size_t off = 0;
    while (off < *n) {
        ssize_t k = write(fd, buf + off, *n - off);
        if (k < 0) { if (errno == EINTR) continue; return -1; }
        off += (size_t)k;
    }
    *n = 0;
    return 0;
}

int writer_main(int pipe_r, const char *dir)
{
    struct sigaction sa = { .sa_handler = on_hup };
    sigemptyset(&sa.sa_mask);
    sigaction(SIGHUP, &sa, NULL);                         /* no SA_RESTART: read() returns EINTR */

    char day[16];
    int fd = open_day(dir, day, sizeof day);
    if (fd < 0) return 2;
    struct stat sb;
    size_t cap = fstat(fd, &sb) == 0 && sb.st_blksize > 0 ? (size_t)sb.st_blksize : 4096;
    char *buf = malloc(cap), in[512];
    if (!buf) return 3;
    size_t used = 0;

    for (;;) {
        ssize_t n = read(pipe_r, in, sizeof in);
        if (n < 0 && errno != EINTR) break;
        if (reopen_requested) {
            /* SIGHUP: write what we have, then reopen (a rotated file is replaced by a new one) */
            reopen_requested = 0;
            flush(fd, buf, &used);
            close(fd);
            if ((fd = open_day(dir, day, sizeof day)) < 0) { free(buf); return 2; }
        }
        if (n == 0) break;                                /* the parent closed the pipe */
        if (n < 0) continue;
        /* a new day starts a new file */
        char today[16];
        time_t now = time(NULL);
        struct tm tm;
        localtime_r(&now, &tm);
        strftime(today, sizeof today, "%Y-%m-%d", &tm);
        if (strcmp(today, day) != 0) { flush(fd, buf, &used); close(fd); if ((fd = open_day(dir, day, sizeof day)) < 0) { free(buf); return 2; } }
        if (used + (size_t)n > cap && flush(fd, buf, &used) < 0) break;
        memcpy(buf + used, in, (size_t)n);
        used += (size_t)n;
        /* keep the file current: flush whenever the pipe is empty (lines are rare, about 10 per second) */
        fd_set rd; FD_ZERO(&rd); FD_SET(pipe_r, &rd);
        struct timeval z = { 0, 0 };
        if (select(pipe_r + 1, &rd, NULL, NULL, &z) == 0 && flush(fd, buf, &used) < 0) break;
    }
    int ok = flush(fd, buf, &used) == 0;
    close(fd);
    free(buf);
    return ok ? 0 : 4;
}
