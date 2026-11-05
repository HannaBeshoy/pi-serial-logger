/*
 * frame.h - the sensor node's line protocol (the same Arduino sketch as the
 * java-serial-monitor project):
 *
 *     $SN,<seq>,<uptime ms>,T=<adc>,D=<echo us>,L=<adc>*<XOR checksum, 2 hex digits>
 *     e.g. $SN,42,123456,T=61,D=2001,L=512*63
 *
 * The checksum is the XOR of every byte between '$' and '*', as in NMEA.
 */
#ifndef FRAME_H
#define FRAME_H
#include <stdbool.h>
#include <stdint.h>

typedef struct {
    uint32_t seq;
    uint32_t uptime_ms;
    int temp_adc;           /* LM35 on a 10-bit ADC at 5 V: degrees C = adc * 500 / 1023 */
    long echo_us;           /* 0 = no echo */
    int light_adc;
} frame_t;

typedef enum { FRAME_OK, FRAME_NO_START, FRAME_NO_STAR, FRAME_BAD_HEX, FRAME_BAD_CHECKSUM, FRAME_BAD_FIELDS } frame_error_t;

uint8_t frame_checksum(const char *start, const char *end);   /* XOR of [start, end) */
frame_error_t frame_parse(const char *line, frame_t *out);
int frame_format(const frame_t *f, char *buf, int cap);       /* with checksum, no newline */
const char *frame_error_name(frame_error_t e);

double frame_temp_c(const frame_t *f);
double frame_dist_cm(const frame_t *f);                       /* negative = no echo */

#endif
