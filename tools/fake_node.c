/*
 * fake_node.c - an Arduino sensor node simulated on a pseudo-terminal, so the
 * logger can be tested on a real tty device (termios and all) without hardware.
 *
 *   ./build/fake_node [-n COUNT] [-t PERIOD_MS] [-e ERROR_PERCENT] [-s SEED]
 *
 * Prints the slave device's path (e.g. /dev/pts/3) on the first line of stdout,
 * then writes one frame per period to it. ERROR_PERCENT of the frames get one
 * corrupted character (the checksum must catch it); every 50th line is noise.
 * At the end it prints "sent N good M corrupted K noise" and exits.
 */
#define _DEFAULT_SOURCE
#define _XOPEN_SOURCE 600
#include <fcntl.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>
#include "../src/frame.h"

int main(int argc, char **argv)
{
    int count = 100, period = 20, err = 5, opt;
    unsigned seed = 1;
    while ((opt = getopt(argc, argv, "n:t:e:s:")) != -1) {
        if (opt == 'n') count = atoi(optarg);
        else if (opt == 't') period = atoi(optarg);
        else if (opt == 'e') err = atoi(optarg);
        else if (opt == 's') seed = (unsigned)atoi(optarg);
        else { fprintf(stderr, "usage: %s [-n count] [-t ms] [-e percent] [-s seed]\n", argv[0]); return 1; }
    }
    int master = posix_openpt(O_RDWR | O_NOCTTY);
    if (master < 0 || grantpt(master) < 0 || unlockpt(master) < 0) { perror("pty"); return 1; }
    printf("%s\n", ptsname(master));
    fflush(stdout);
    /* keep a slave fd open ourselves, so writes do not fail before the logger opens it */
    int slave = open(ptsname(master), O_RDWR | O_NOCTTY);
    struct termios t;
    tcgetattr(slave, &t); cfmakeraw(&t); tcsetattr(slave, TCSANOW, &t);
    sleep(1);                                            /* give the logger time to open the device */
    srand(seed);
    int good = 0, bad = 0, noise = 0;
    for (int i = 0; i < count; i++) {
        char buf[128];
        if (i % 50 == 49) { int n = snprintf(buf, sizeof buf, "boot: hello from the node\r\n"); (void)!write(master, buf, n); noise++; }
        frame_t f = { (uint32_t)i, (uint32_t)(i * period), (int)(47 + 10 * sin(i / 20.0)) + rand() % 3,
                      rand() % 20 == 0 ? 0 : 1000 + rand() % 3000, 300 + rand() % 400 };
        int n = frame_format(&f, buf, sizeof buf - 2);
        if (rand() % 100 < err) { buf[5 + rand() % (n - 9)] ^= 0x01; bad++; } else good++;   /* flip one bit */
        buf[n++] = '\r'; buf[n++] = '\n';
        (void)!write(master, buf, n);
        usleep(period * 1000);
    }
    tcdrain(master);
    usleep(200000);
    printf("sent %d good %d corrupted %d noise %d\n", good + bad, good, bad, noise);
    fflush(stdout);
    close(slave);
    close(master);
    return 0;
}
