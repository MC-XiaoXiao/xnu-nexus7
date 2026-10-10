/* SPDX-License-Identifier: BSD-2-Clause */
#include "shim.h"
void *memcpy(void *dest, const void *source, size_t n)
{
    unsigned char *d = dest; const unsigned char *s = source;
    while (n--) *d++ = *s++;
    return dest;
}
void *memset(void *dest, int c, size_t n)
{
    unsigned char *d = dest;
    while (n--) *d++ = (unsigned char)c;
    return dest;
}
size_t shim_strlen(const char *s, size_t max)
{
    size_t i = 0;
    while (i < max && s[i]) ++i;
    return i;
}
bool shim_range_valid(struct shim_range r)
{
    return r.size && r.base <= UINT32_MAX - r.size;
}
bool shim_overlap(struct shim_range a, struct shim_range b)
{
    return a.size && b.size && (uint64_t)a.base < (uint64_t)b.base + b.size &&
           (uint64_t)b.base < (uint64_t)a.base + a.size;
}
bool shim_hex(const char *s, uint32_t *out)
{
    uint32_t v = 0; unsigned int count = 0;
    if (!s) return false;
    if (s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) s += 2;
    for (; *s; ++s) {
        unsigned int c = (unsigned char)*s, digit;
        if (++count > 8) return false;
        if (c >= '0' && c <= '9') digit = c - '0';
        else if (c >= 'a' && c <= 'f') digit = c - 'a' + 10;
        else if (c >= 'A' && c <= 'F') digit = c - 'A' + 10;
        else return false;
        v = (v << 4) | digit;
    }
    if (!count) return false;
    *out = v;
    return true;
}
