/* SPDX-License-Identifier: BSD-2-Clause */
#include "shim.h"
#include <pexpert/apple_dt.h>
static void put32(unsigned char *p,uint32_t v)
{ for (unsigned i=0;i<4;++i) p[i]=(unsigned char)(v>>(8*i)); }
static bool pair(void *tree,const char *path,const char *key,uint32_t a,uint32_t b)
{
    uint32_t size;
    unsigned char *p=(unsigned char *)ADTProperty(tree,path,key,&size);
    if (!p || size!=8) return false;
    put32(p,a); put32(p+4,b); return true;
}
bool shim_dt_compatible(const unsigned char *list,uint32_t size,const char *expected)
{
    bool found=false;
    size_t want=shim_strlen(expected,128);
    uint32_t offset=0;
    if (!list || !size || !want || want==128) return false;
    while (offset<size) {
        uint32_t length=0;
        while (length<size-offset && list[offset+length]) ++length;
        if (!length || length==size-offset) return false;
        if (length==want && ADTEqual(list+offset,expected)) found=true;
        offset+=length+1;
    }
    return found;
}
const char *xnu_prepare_tree(void *buffer,size_t capacity,size_t *used,const void *source,size_t size,
                           const struct shim_identity *id,const struct shim_policy *p,
                           const struct macho_plan *m,const struct xnu_layout *l)
{
    uint32_t n;
    if (size>capacity || size>SHIM_DT_SIZE || !ADTValid(source,size)) return "invalid Apple device tree";
    const unsigned char *compat=ADTProperty(source,"/","compatible",&n);
    if (!shim_dt_compatible(compat,n,id->compatible))
        return "device tree platform mismatch";
    const unsigned char *uart=ADTProperty(source,"/arm-io/serial","reg",&n);
    if (!uart || n!=8 || ADTRead(uart)!=id->uart) return "device tree UART mismatch";
    memcpy(buffer,source,size);
    if (!pair(buffer,"/memory","reg",p->phys_base,p->mem_size) ||
        !pair(buffer,"/chosen/memory-map","Kernel",p->phys_base,m->end_pa-p->phys_base) ||
        !pair(buffer,"/chosen/memory-map","BootArgs",l->args_pa,sizeof(shim_boot_args)) ||
        !pair(buffer,"/chosen/memory-map","DeviceTree",l->dt_pa,(uint32_t)size))
        return "device tree lacks boot fixup slots";
    *used=size;
    return NULL;
}
