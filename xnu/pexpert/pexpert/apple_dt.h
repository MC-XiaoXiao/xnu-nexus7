/* SPDX-License-Identifier: BSD-2-Clause */
#ifndef _PEXPERT_APPLE_DT_H_
#define _PEXPERT_APPLE_DT_H_
/* Allocation-free, bytewise access: usable by both the loader and early XNU.
 * Call ADTValid before ADTProperty. This is Apple DT, not big-endian FDT. */
#if defined(KERNEL)
#include <sys/types.h>
#else
#include <stdint.h>
#include <stddef.h>
#endif
static inline uint32_t ADTRead(const unsigned char *p)
{ return (uint32_t)p[0] | (uint32_t)p[1]<<8 | (uint32_t)p[2]<<16 | (uint32_t)p[3]<<24; }
static inline int ADTEqual(const unsigned char *p, const char *s)
{
    while (*s && *p == (unsigned char)*s) { ++p; ++s; }
    return *p == 0 && *s == 0;
}
static inline int ADTNode(const unsigned char *b, size_t len, size_t *pos, unsigned depth)
{
    size_t start = *pos, first, previous, i, j, padded;
    uint32_t props, children, n;
    int named = 0;
    if (depth > 32 || start > len || len-start < 8) return 0;
    props = ADTRead(b+start); children = ADTRead(b+start+4);
    *pos += 8; first = *pos;
    if (!props || props > (len-*pos)/36) return 0;
    for (i=0; i<props; ++i) {
        if (len-*pos < 36) return 0;
        for (j=0; j<32 && b[*pos+j]; ++j) {}
        if (!j || j==32) return 0;
        previous = first;
        while (previous < *pos) {
            if (ADTEqual(b+previous, (const char *)b+*pos)) return 0;
            previous += 36 + ((size_t)ADTRead(b+previous+32)+3)/4*4;
        }
        n = ADTRead(b+*pos+32);
        if (n > len-*pos-36) return 0;
        padded = ((size_t)n+3)/4*4;
        if (padded > len-*pos-36) return 0;
        if (ADTEqual(b+*pos,"model") || ADTEqual(b+*pos,"compatible") ||
            ADTEqual(b+*pos,"device_type") || ADTEqual(b+*pos,"firmware-version") ||
            ADTEqual(b+*pos,"stdout-path")) {
            if (!n || b[*pos+36+n-1]) return 0;
        }
        if (ADTEqual(b+*pos,"name")) {
            if (!n || n>32 || !b[*pos+36] || b[*pos+36+n-1]) return 0;
            for (j=0; j+1<n; ++j) if (!b[*pos+36+j] || b[*pos+36+j]=='/') return 0;
            named = 1;
        }
        *pos += 36+padded;
    }
    if (!named || children > (len-*pos)/8) return 0;
    for (i=0; i<children; ++i) if (!ADTNode(b,len,pos,depth+1)) return 0;
    return 1;
}
static inline int ADTValid(const void *tree, size_t len)
{
    size_t pos=0;
    return tree && len && len <= 0x100000 && ADTNode(tree,len,&pos,0) && pos==len;
}
static inline size_t ADTSkip(const unsigned char *b, size_t pos)
{
    uint32_t props=ADTRead(b+pos), children=ADTRead(b+pos+4);
    pos+=8;
    while (props--) pos+=36+((size_t)ADTRead(b+pos+32)+3)/4*4;
    while (children--) pos=ADTSkip(b,pos);
    return pos;
}
static inline const unsigned char *ADTLocal(const unsigned char *b, size_t pos,
                                           const char *key, uint32_t *size)
{
    uint32_t props=ADTRead(b+pos); pos+=8;
    while (props--) {
        uint32_t n=ADTRead(b+pos+32);
        if (ADTEqual(b+pos,key)) { *size=n; return b+pos+36; }
        pos+=36+((size_t)n+3)/4*4;
    }
    return NULL;
}
static inline const unsigned char *ADTProperty(const void *tree, const char *path,
                                              const char *key, uint32_t *size)
{
    const unsigned char *b=tree;
    size_t node=0;
    if (*path!='/') return NULL;
    ++path;
    while (*path) {
        char part[32]; size_t n=0, child=node+8;
        uint32_t props=ADTRead(b+node), count=ADTRead(b+node+4), len;
        while (*path && *path!='/') { if (n==31) return NULL; part[n++]=*path++; }
        part[n]=0;
        if (*path=='/') ++path;
        while (props--) child+=36+((size_t)ADTRead(b+child+32)+3)/4*4;
        while (count) {
            const unsigned char *name=ADTLocal(b,child,"name",&len);
            if (name && ADTEqual(name,part)) break;
            child=ADTSkip(b,child); --count;
        }
        if (!count) return NULL;
        node=child;
    }
    return ADTLocal(b,node,key,size);
}
#endif
