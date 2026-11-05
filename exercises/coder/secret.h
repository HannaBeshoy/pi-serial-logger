/*
 * secret.h - coding and decoding for the course's "complex practicing task".
 *
 * Coding: in every byte the first two bits (b7 b6) and the last two bits (b1 b0)
 * change places, then a random English letter is inserted after it. Decoding
 * drops every second byte and swaps the bits back (the swap is its own inverse).
 */
#ifndef SECRET_H
#define SECRET_H
#include <stddef.h>
#include <stdlib.h>

static inline unsigned char swap_ends(unsigned char c)
{
    return (unsigned char)((c & 0x3C) | ((c & 0xC0) >> 6) | ((c & 0x03) << 6));
}

/* out must hold 2 * n bytes; returns the number written */
static inline size_t code_block(const unsigned char *in, size_t n, unsigned char *out)
{
    static const char letters[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz";
    for (size_t i = 0; i < n; i++) {
        out[2 * i] = swap_ends(in[i]);
        out[2 * i + 1] = (unsigned char)letters[rand() % 52];
    }
    return 2 * n;
}

/* *odd carries the position parity across blocks (1 = the next byte is an inserted letter) */
static inline size_t decode_block(const unsigned char *in, size_t n, unsigned char *out, int *odd)
{
    size_t k = 0;
    for (size_t i = 0; i < n; i++, *odd ^= 1)
        if (!*odd) out[k++] = swap_ends(in[i]);
    return k;
}
#endif
