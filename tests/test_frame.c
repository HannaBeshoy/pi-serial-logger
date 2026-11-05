/* Unit tests for frame.c: make test */
#include <stdio.h>
#include <string.h>
#include "../src/frame.h"

static int checks, failed;
#define CHECK(c) do { checks++; if (!(c)) { failed++; printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); } } while (0)

int main(void)
{
    frame_t f;
    /* the example from the java-serial-monitor protocol document */
    CHECK(frame_parse("$SN,42,123456,T=61,D=2001,L=512*63", &f) == FRAME_OK);
    CHECK(f.seq == 42 && f.uptime_ms == 123456 && f.temp_adc == 61 && f.echo_us == 2001 && f.light_adc == 512);
    CHECK(frame_parse("noise$SN,42,123456,T=61,D=2001,L=512*63\r\n", &f) == FRAME_OK);   /* junk before '$', CRLF after */
    CHECK(frame_parse("$SN,42,123456,T=61,D=2001,L=512*64", &f) == FRAME_BAD_CHECKSUM);
    CHECK(frame_parse("$SN,42,123456,T=61,D=2001,L=513*63", &f) == FRAME_BAD_CHECKSUM);  /* one flipped digit */
    CHECK(frame_parse("$SN,42,123456,T=61,D=2001,L=512*6", &f) == FRAME_BAD_HEX);
    CHECK(frame_parse("$SN,42,123456,T=61,D=2001,L=512", &f) == FRAME_NO_STAR);
    CHECK(frame_parse("SN,42", &f) == FRAME_NO_START);
    /* a frame with a valid checksum but nonsense fields */
    char buf[96];
    frame_t g = { 7, 1000, 2000, 5, 10 };                 /* temp_adc out of range */
    frame_format(&g, buf, sizeof buf);
    CHECK(frame_parse(buf, &f) == FRAME_BAD_FIELDS);
    /* format and parse agree for many values */
    int ok = 1;
    for (uint32_t i = 0; i < 5000; i++) {
        frame_t a = { i * 7919u, i * 200u, (int)(i % 1024), (long)(i * 3 % 30000), (int)((i * 13) % 1024) };
        frame_t b;
        ok &= frame_format(&a, buf, sizeof buf) > 0 && frame_parse(buf, &b) == FRAME_OK
              && memcmp(&a, &b, sizeof a) == 0;
    }
    CHECK(ok);
    /* every single-bit error in the body is caught (XOR detects all of them) */
    frame_t c = { 42, 123456, 61, 2001, 512 };
    int n = frame_format(&c, buf, sizeof buf), caught = 0, tried = 0;
    for (int pos = 1; pos < n - 3; pos++)
        for (int bit = 0; bit < 7; bit++) {
            char m[96];
            memcpy(m, buf, n + 1);
            m[pos] ^= (char)(1 << bit);
            if (m[pos] == '*' || m[pos] == '$' || m[pos] == 0) continue;
            tried++;
            caught += frame_parse(m, &f) != FRAME_OK;
        }
    CHECK(caught == tried && tried > 100);
    CHECK(frame_temp_c(&c) > 29.8 && frame_temp_c(&c) < 29.9);   /* 61 * 500 / 1023 = 29.81 */
    c.echo_us = 0;
    CHECK(frame_dist_cm(&c) < 0);
    printf("%d checks, %d failed\n", checks, failed);
    return failed != 0;
}
