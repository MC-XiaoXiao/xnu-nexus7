/* SPDX-License-Identifier: BSD-2-Clause */
#include "shim.h"
const char *xnu_layout(const struct macho_plan *m, const struct shim_policy *p, struct xnu_layout *out)
{
    /* boot_args and DT must precede topOfKernelData: arm_vm_init allocates its
     * own page tables and later free pages starting there. Use wide arithmetic. */
    uint64_t args = ((uint64_t)m->end_pa+15)&~15ULL;
    uint64_t dt = (args+sizeof(shim_boot_args)+0xfff)&~0xfffULL;
    uint64_t top = (dt+SHIM_DT_SIZE+0x3fff)&~0x3fffULL;
    uint64_t end = top+SHIM_VM_RESERVE;
    if (end > (uint64_t)p->phys_base+p->mem_size || end > UINT32_MAX ||
        m->end_pa < p->phys_base) return "insufficient RAM for boot metadata and VM tables";
    struct shim_range reserve = {m->end_pa,(uint32_t)(end-m->end_pa)};
    if (shim_overlap(reserve,p->source) || shim_overlap(reserve,p->shim))
        return "boot metadata overwrites source or shim";
    *out=(struct xnu_layout){(uint32_t)args,(uint32_t)dt,(uint32_t)top,(uint32_t)end};
    return NULL;
}
const char *xnu_build_args(shim_boot_args *a, const struct shim_policy *p,
                          const struct xnu_layout *l, uint32_t dt_size, const char *command)
{
    size_t n = shim_strlen(command,SHIM_CMDLINE_SIZE);
    if (n == SHIM_CMDLINE_SIZE) return "XNU command line exceeds 255 bytes";
    if (!dt_size || dt_size > SHIM_DT_SIZE || l->dt_pa < p->phys_base ||
        l->dt_pa-p->phys_base >= p->mem_size ||
        dt_size > p->mem_size-(l->dt_pa-p->phys_base) ||
        (uint64_t)p->virt_base+(l->dt_pa-p->phys_base)+dt_size > UINT32_MAX) return "invalid device tree";
    memset(a,0,sizeof(*a));
    a->Revision=1; a->Version=2;
    a->physBase=p->phys_base; a->virtBase=p->virt_base; a->memSize=p->mem_size;
    a->topOfKernelData=l->ttb_pa;
    /* locore translates r0 but not this field, so it must already be a VA. */
#if defined(__arm__)
    a->deviceTreeP=(void *)(uintptr_t)(p->virt_base+(l->dt_pa-p->phys_base));
#else
    a->deviceTreeP=p->virt_base+(l->dt_pa-p->phys_base);
#endif
    a->deviceTreeLength=dt_size;
    memcpy(a->CommandLine,command,n+1);
    return NULL;
}

/* The framebuffer stays outside kernel-managed RAM; Video carries a PA. */
#include <pexpert/apple_dt.h>
const char *xnu_set_framebuffer(shim_boot_args *a, void *tree,
    const struct shim_policy *p, const struct shim_framebuffer *f)
{
    uint64_t bytes = (uint64_t)f->stride * f->height;
    struct shim_range range = { f->base, (uint32_t)bytes };
    if (!f->base || (f->base & 3) || !f->width || !f->height ||
        f->width > 8192 || f->height > 8192 ||
        (f->depth != 16 && f->depth != 32) ||
        (f->stride & 3) || f->stride < f->width * (f->depth / 8) ||
        !bytes || bytes > 0x2000000 || !shim_range_valid(range) ||
        shim_overlap(range, (struct shim_range){p->phys_base,p->mem_size}) ||
        shim_overlap(range,p->source) || shim_overlap(range,p->shim))
        return "invalid or overlapping framebuffer";
    uint32_t size;
    void *map = (void *)ADTProperty(tree,"/chosen/memory-map","Framebuffer",&size);
    if (!map || size != 8) return "device tree lacks framebuffer fixup slot";
    uint32_t pair[2] = {f->base,(uint32_t)bytes};
    memcpy(map,pair,sizeof(pair));
    a->Video.v_baseAddr=f->base;
    a->Video.v_display=1;
    a->Video.v_rowBytes=f->stride;
    a->Video.v_width=f->width;
    a->Video.v_height=f->height;
    a->Video.v_depth=f->depth;
    return NULL;
}
