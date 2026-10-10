/* SPDX-License-Identifier: BSD-2-Clause */
#include "shim.h"
extern unsigned char __shim_start[], __shim_end[];
static struct macho_plan image_plan;
static shim_boot_args args;
static unsigned char tree[SHIM_DT_SIZE];
static char command[SHIM_CMDLINE_SIZE];
static void puts_uart(const char *s) { while (*s) platform_putc(*s++); }
static void hex_uart(uint32_t n)
{
    puts_uart("0x");
    for (int i=28;i>=0;i-=4) platform_putc("0123456789abcdef"[(n>>i)&15]);
}
static __attribute__((noreturn)) void fail(const char *s) { puts_uart("shim: ERROR: "); puts_uart(s); puts_uart("\n"); shim_halt(); }
static void *physical_map(void *context,uint32_t address,uint32_t size)
{ (void)context; (void)size; return (void *)(uintptr_t)address; }
/* U-Boot go and bootelf both call this entry using argc/argv, not Linux ABI.
 * Startup installs our stack and masks IRQ/FIQ. We take ownership permanently. */
void shim_main(int argc, const char *const argv[])
{
    uint32_t address, size, ram_size, dt_address, dt_size;
    puts_uart("\nXNU standalone shim v1\n");
    if (argc != 7 && argc != 12) fail("usage: go SHIM KERNEL_HEX SIZE_HEX RAM_SIZE_HEX \"XNU arguments\" DT_HEX DT_SIZE_HEX [FB_HEX STRIDE_HEX WIDTH_HEX HEIGHT_HEX DEPTH_HEX]");
    if (!shim_hex(argv[1],&address) || !shim_hex(argv[2],&size) || !shim_hex(argv[3],&ram_size) ||
        !shim_hex(argv[5],&dt_address) || !shim_hex(argv[6],&dt_size))
        fail("addresses and sizes must be hexadecimal");
    size_t cmd_size=shim_strlen(argv[4],sizeof(command));
    if (cmd_size == sizeof(command)) fail("command line exceeds 255 bytes");
    memcpy(command,argv[4],cmd_size+1);
    struct shim_policy p = {
        .phys_base=platform_ram_base(), .virt_base=platform_virtual_base(), .mem_size=ram_size,
        .source={address,size},
        .shim={(uint32_t)(uintptr_t)__shim_start,(uint32_t)(__shim_end-__shim_start)}
    };
    if (ram_size < 0x1000000U || ram_size > 0x8000000U || (ram_size & 0xfffffU))
        fail("RAM size must be 16..128 MiB, a multiple of 1 MiB");
    /* Source is staged outside all memory handed to XNU. This also avoids
     * destroying the file when kernel VM initialization starts using pages. */
    if (!shim_range_valid(p.source) || size > 0x2000000U ||
        address < p.phys_base+0x8000000U || address >= platform_ram_limit() ||
        size > platform_ram_limit()-address || shim_overlap(p.source,p.shim))
        fail("invalid kernel staging range");
    if (!armv7_identity_range(p.shim.base,p.shim.size) ||
        !armv7_identity_range(p.source.base,p.source.size) ||
        !armv7_identity_range(p.phys_base,p.mem_size))
        fail("U-Boot RAM mappings must be identity mappings");
    struct shim_range dt_source={dt_address,dt_size};
    if (!dt_size || dt_size>SHIM_DT_SIZE || !shim_range_valid(dt_source) ||
        dt_address<p.phys_base+0x8000000U || dt_address>=platform_ram_limit() ||
        dt_size>platform_ram_limit()-dt_address || shim_overlap(dt_source,p.source) ||
        shim_overlap(dt_source,p.shim) || !armv7_identity_range(dt_address,dt_size))
        fail("invalid device tree staging range");
    const void *image=(const void *)(uintptr_t)address;
    const char *error=macho_plan(image,size,&p,&image_plan);
    if (error) fail(error);
    struct xnu_layout layout;
    error=xnu_layout(&image_plan,&p,&layout); if (error) fail(error);
    struct shim_identity id=platform_identity();
    size_t tree_size;
    error=xnu_prepare_tree(tree,sizeof(tree),&tree_size,(const void *)(uintptr_t)dt_address,dt_size,
                           &id,&p,&image_plan,&layout);
    if (error) fail(error);
    error=xnu_build_args(&args,&p,&layout,(uint32_t)tree_size,command);
    if (error) fail(error);
    if (argc == 12) {
        struct shim_framebuffer fb;
        if (!shim_hex(argv[7],&fb.base) || !shim_hex(argv[8],&fb.stride) ||
            !shim_hex(argv[9],&fb.width) || !shim_hex(argv[10],&fb.height) ||
            !shim_hex(argv[11],&fb.depth)) fail("invalid framebuffer arguments");
        uint64_t end = (uint64_t)fb.base + (uint64_t)fb.stride * fb.height;
        if (fb.base < platform_ram_base() || end > platform_ram_limit())
            fail("framebuffer outside physical RAM");
        error=xnu_set_framebuffer(&args,tree,&p,&fb); if (error) fail(error);
    }
    puts_uart("shim: entry PA "); hex_uart(image_plan.entry_pa);
    puts_uart(", boot_args "); hex_uart(layout.args_pa);
    puts_uart(", TTB "); hex_uart(layout.ttb_pa);
    puts_uart("\nshim: validating platform and taking ownership\n");
    error=platform_prepare(argc == 12); if (error) fail(error);
    if (armv7_normalize(platform_outer_cache_base())) fail("cache shutdown timeout");
    if (!macho_load(image,&image_plan,physical_map,NULL)) fail("cannot map segment destinations");
    /* Zero metadata padding and the bootstrap L1 only. Kernel owns everything
     * above it and allocates further VM page tables there. */
    memset((void *)(uintptr_t)image_plan.end_pa,0,layout.ttb_pa+0x4000U-image_plan.end_pa);
    memcpy((void *)(uintptr_t)layout.args_pa,&args,sizeof(args));
    memcpy((void *)(uintptr_t)layout.dt_pa,tree,tree_size);
    puts_uart("shim: jumping to XNU (ARM, MMU/cache off, IRQ/FIQ masked)\n");
    platform_drain_uart();
    armv7_enter(image_plan.entry_pa,layout.args_pa);
}
void shim_halt(void)
{
    for (;;) __asm__ volatile("cpsid if\n\tdsb sy\n\twfi" ::: "memory");
}
