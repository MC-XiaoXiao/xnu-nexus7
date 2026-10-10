/* SPDX-License-Identifier: BSD-2-Clause */
#include "shim.h"
/* Decode bytes rather than casting: malformed/unaligned input is safe on hosts
 * and on ARM. The supported subset is fixed-address, little-endian ARMv7 XNU. */
static uint32_t word(const unsigned char *p)
{ return p[0] | (uint32_t)p[1]<<8 | (uint32_t)p[2]<<16 | (uint32_t)p[3]<<24; }
const char *macho_plan(const void *image, size_t size, const struct shim_policy *p, struct macho_plan *out)
{
    const unsigned char *b = image;
    struct macho_plan plan = {0};
    uint32_t pos = 28, commands, end, threads = 0;
    if (!shim_range_valid(p->source) || size != p->source.size || size < 28)
        return "invalid image size";
    if (!shim_range_valid((struct shim_range){p->phys_base, p->mem_size}) ||
        !shim_range_valid((struct shim_range){p->virt_base, p->mem_size}) ||
        (p->phys_base | p->virt_base | p->mem_size) & 0xfffffU ||
        !shim_range_valid(p->shim)) return "invalid RAM policy";
    if (shim_overlap(p->source, p->shim)) return "source overlaps shim";
    if (word(b) != 0xfeedface || word(b+4) != 12 ||
        (word(b+8) & 0xffffffU) != 9 || word(b+12) != 2)
        return "expected ARMv7 little-endian MH_EXECUTE";
    /* XNU carries MH_PIE but is loaded at its linked VAs, with no slide. */
    if (!(word(b+24) & 1)) return "image has unresolved symbols";
    commands = word(b+16);
    if (word(b+20) > size-28 || commands > word(b+20)/8)
        return "invalid load command table";
    end = 28 + word(b+20);
    for (uint32_t i=0; i<commands; ++i) {
        if (pos > end || end-pos < 8) return "truncated load command";
        uint32_t cmd = word(b+pos), n = word(b+pos+4);
        if (n < 8 || (n & 3) || n > end-pos) return "invalid load command size";
        if (cmd == 1) { /* LC_SEGMENT */
            if (n < 56 || word(b+pos+48) > (n-56)/68 ||
                n != 56 + word(b+pos+48)*68) return "invalid segment sections";
            struct macho_segment s = {
                .va=word(b+pos+24), .memsz=word(b+pos+28),
                .offset=word(b+pos+32), .filesz=word(b+pos+36), .prot=word(b+pos+44)
            };
            if (s.filesz > s.memsz || s.offset > size || s.filesz > size-s.offset)
                return "segment exceeds file";
            if (!s.memsz) { pos += n; continue; }
            if (plan.count == SHIM_MAX_SEGMENTS || s.va < p->virt_base ||
                s.va-p->virt_base >= p->mem_size ||
                s.memsz > p->mem_size-(s.va-p->virt_base)) return "segment outside RAM";
            s.pa = p->phys_base + (s.va-p->virt_base);
            struct shim_range dest = {s.pa, s.memsz};
            if (shim_overlap(dest,p->source) || shim_overlap(dest,p->shim))
                return "segment overwrites source or shim";
            for (uint32_t j=0; j<plan.count; ++j)
                if (shim_overlap(dest,(struct shim_range){plan.segments[j].pa, plan.segments[j].memsz}))
                    return "overlapping segments";
            plan.segments[plan.count++] = s;
            if (s.pa+s.memsz > plan.end_pa) plan.end_pa = s.pa+s.memsz;
        } else if (cmd == 5) { /* LC_UNIXTHREAD: ARM_THREAD_STATE */
            if (++threads != 1 || n != 84 || word(b+pos+8) != 1 || word(b+pos+12) != 17)
                return "unsupported ARM entry state";
            plan.entry_va = word(b+pos+76);
            if (plan.entry_va & 3) return "entry must be ARM-aligned";
        } else if (cmd != 2 && cmd != 0xb && cmd != 0x1b && cmd != 0x2a &&
                   cmd != 0x26 && cmd != 0x24 && cmd != 0x25 && cmd != 0x29 && cmd != 0x32) {
            return "unsupported Mach-O load command";
        }
        pos += n;
    }
    if (pos != end || !plan.count || threads != 1) return "missing segments or entry";
    bool executable = false;
    for (uint32_t j=0; j<plan.count; ++j) {
        struct macho_segment *s = &plan.segments[j];
        /* Require an actual file-backed ARM instruction, not executable BSS. */
        if ((s->prot & 4) && s->filesz >= 4 && plan.entry_va >= s->va &&
            plan.entry_va-s->va <= s->filesz-4) {
            executable = true; plan.entry_pa = s->pa + (plan.entry_va-s->va);
        }
    }
    if (!executable) return "entry is not file-backed executable memory";
    *out = plan; /* Never publish a partial plan on failure. */
    return NULL;
}
bool macho_load(const void *image, const struct macho_plan *p, shim_map_fn map, void *ctx)
{
    void *dest[SHIM_MAX_SEGMENTS];
    if (!p->count || p->count > SHIM_MAX_SEGMENTS) return false;
    /* Resolve all destinations before the first write. */
    for (uint32_t i=0; i<p->count; ++i) {
        dest[i] = map(ctx,p->segments[i].pa,p->segments[i].memsz);
        if (!dest[i]) return false;
    }
    for (uint32_t i=0; i<p->count; ++i) {
        const struct macho_segment *s=&p->segments[i];
        memcpy(dest[i],(const unsigned char *)image+s->offset,s->filesz);
        memset((unsigned char *)dest[i]+s->filesz,0,s->memsz-s->filesz);
    }
    return true;
}
