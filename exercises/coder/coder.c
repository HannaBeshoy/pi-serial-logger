/*
 * coder.c - the System Programming course's complex practicing task.
 *
 *   coder -c    code a file from $HOME        coder -d    decode it
 *
 * 1. "-c" codes, "-d" decodes; anything else: a message on stderr and exit code 1.
 * 2. Lists only the regular files in $HOME, then reads a name from standard input,
 *    asking again until it is an existing regular file there.
 * 3. The coding itself is in secret.h.
 * 4. The result replaces the input file, through a temporary file in the same directory.
 * 5. Reads and writes with a buffer as large as the input file's st_blksize (malloc).
 * 6. Ctrl-C is ignored for the first 10 seconds of the coding.
 */
#define _DEFAULT_SOURCE
#include <dirent.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
#include "secret.h"

static void restore_sigint(int sig) { (void)sig; signal(SIGINT, SIG_DFL); }

static int is_regular(const char *dir, const char *name)
{
    char path[4096];
    struct stat sb;
    if (strchr(name, '/') || !*name) return 0;                   /* only names in that directory */
    snprintf(path, sizeof path, "%s/%s", dir, name);
    return lstat(path, &sb) == 0 && S_ISREG(sb.st_mode);
}

int main(int argc, char **argv)
{
    if (argc != 2 || (strcmp(argv[1], "-c") != 0 && strcmp(argv[1], "-d") != 0)) {
        fprintf(stderr, "usage: %s -c | -d   (code or decode a file of your home directory)\n", argv[0]);
        return 1;
    }
    int coding = argv[1][1] == 'c';
    const char *home = getenv("HOME");
    if (!home) { fprintf(stderr, "HOME is not set\n"); return 1; }

    DIR *d = opendir(home);
    if (!d) { perror(home); return 1; }
    printf("Regular files in %s:\n", home);
    for (struct dirent *e; (e = readdir(d));)
        if (is_regular(home, e->d_name)) printf("  %s\n", e->d_name);
    closedir(d);

    char name[256];
    for (;;) {
        printf("File to %s: ", coding ? "code" : "decode");
        fflush(stdout);
        if (!fgets(name, sizeof name, stdin)) { fprintf(stderr, "no file chosen\n"); return 1; }
        name[strcspn(name, "\r\n")] = '\0';
        if (is_regular(home, name)) break;
        printf("'%s' is not a regular file in %s, try again.\n", name, home);
    }

    char path[4096], tmp[4096];
    snprintf(path, sizeof path, "%s/%s", home, name);
    snprintf(tmp, sizeof tmp, "%s/.%s.XXXXXX", home, name);
    int in = open(path, O_RDONLY);
    struct stat sb;
    if (in < 0 || fstat(in, &sb) < 0) { perror(path); return 1; }
    int out = mkstemp(tmp);                                      /* same directory: rename() is atomic */
    if (out < 0) { perror("temporary file"); return 1; }

    size_t bs = sb.st_blksize > 0 ? (size_t)sb.st_blksize : 4096;
    unsigned char *buf = malloc(bs), *res = malloc(2 * bs);
    if (!buf || !res) { fprintf(stderr, "out of memory\n"); return 1; }

    signal(SIGINT, SIG_IGN);                                     /* not interruptible for 10 s */
    signal(SIGALRM, restore_sigint);
    alarm(10);
    srand((unsigned)time(NULL) ^ (unsigned)getpid());

    int odd = 0, ok = 1;
    ssize_t n;
    while ((n = read(in, buf, bs)) > 0) {
        size_t m = coding ? code_block(buf, (size_t)n, res) : decode_block(buf, (size_t)n, res, &odd);
        if (write(out, res, m) != (ssize_t)m) { perror("write"); ok = 0; break; }
    }
    if (n < 0) { perror("read"); ok = 0; }
    close(in);
    if (close(out) < 0) ok = 0;
    if (ok && (chmod(tmp, sb.st_mode & 07777) < 0 || rename(tmp, path) < 0)) { perror("rename"); ok = 0; }
    if (!ok) unlink(tmp);
    free(buf); free(res);
    printf("%s %s: %s\n", coding ? "coded" : "decoded", name, ok ? "done" : "failed");
    return ok ? 0 : 1;
}
