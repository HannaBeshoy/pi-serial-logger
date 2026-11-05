#include "frame.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

uint8_t frame_checksum(const char *start, const char *end)
{
    uint8_t x = 0;
    for (const char *p = start; p < end; p++) x ^= (uint8_t)*p;
    return x;
}

static int hex(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    return -1;
}

frame_error_t frame_parse(const char *line, frame_t *out)
{
    const char *start = strchr(line, '$');
    if (!start) return FRAME_NO_START;
    const char *star = strchr(start, '*');
    if (!star) return FRAME_NO_STAR;
    int h = hex(star[1]), l = h < 0 ? -1 : hex(star[2]);
    if (h < 0 || l < 0) return FRAME_BAD_HEX;
    if (frame_checksum(start + 1, star) != (uint8_t)(h << 4 | l)) return FRAME_BAD_CHECKSUM;

    char body[96];
    size_t n = (size_t)(star - start - 1);
    if (n >= sizeof body) return FRAME_BAD_FIELDS;
    memcpy(body, start + 1, n);
    body[n] = '\0';
    unsigned long seq, up;
    int t, lgt, used = 0;
    long d;
    if (sscanf(body, "SN,%lu,%lu,T=%d,D=%ld,L=%d%n", &seq, &up, &t, &d, &lgt, &used) != 5 || (size_t)used != n)
        return FRAME_BAD_FIELDS;
    if (t < 0 || t > 1023 || lgt < 0 || lgt > 1023 || d < 0 || d > 1000000) return FRAME_BAD_FIELDS;
    out->seq = (uint32_t)seq; out->uptime_ms = (uint32_t)up;
    out->temp_adc = t; out->echo_us = d; out->light_adc = lgt;
    return FRAME_OK;
}

int frame_format(const frame_t *f, char *buf, int cap)
{
    int n = snprintf(buf, cap, "$SN,%lu,%lu,T=%d,D=%ld,L=%d", (unsigned long)f->seq, (unsigned long)f->uptime_ms,
                     f->temp_adc, f->echo_us, f->light_adc);
    if (n < 0 || n + 3 >= cap) return -1;
    return n + snprintf(buf + n, cap - n, "*%02X", frame_checksum(buf + 1, buf + n));
}

const char *frame_error_name(frame_error_t e)
{
    static const char *names[] = { "ok", "no '$'", "no '*'", "bad checksum digits", "checksum mismatch", "bad fields" };
    return names[e];
}

double frame_temp_c(const frame_t *f) { return f->temp_adc * 500.0 / 1023.0; }
double frame_dist_cm(const frame_t *f) { return f->echo_us ? f->echo_us * 0.0343 / 2.0 : -1.0; }
