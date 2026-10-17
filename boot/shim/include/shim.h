/* SPDX-License-Identifier: BSD-2-Clause */
#ifndef XNU_SHIM_H
#define XNU_SHIM_H
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#define SHIM_MAX_SEGMENTS 32
#define SHIM_CMDLINE_SIZE 256
#define SHIM_DT_SIZE 8192
#define SHIM_VM_RESERVE 0x200000U
struct shim_range { uint32_t base, size; };
struct macho_segment { uint32_t va, pa, memsz, offset, filesz, prot; };
struct macho_plan {
    struct macho_segment segments[SHIM_MAX_SEGMENTS];
    uint32_t count, entry_va, entry_pa, end_pa;
};
struct shim_policy {
    uint32_t phys_base, virt_base, mem_size;
    struct shim_range source, shim;
};
/* Use XNU's real ABI on ARM. Host tests need 32-bit pointer storage even on
 * 64-bit hosts; tools/test-xnu-shim.py checks this mirror against boot.h. */
#if defined(__arm__) && !defined(SHIM_HOST_ABI)
#include <pexpert/arm/boot.h>
typedef boot_args shim_boot_args;
#else
typedef struct {
    uint16_t Revision, Version;
    uint32_t virtBase, physBase, memSize, topOfKernelData;
    struct { uint32_t v_baseAddr, v_display, v_rowBytes, v_width, v_height, v_depth; } Video;
    uint32_t machineType, deviceTreeP, deviceTreeLength;
    char CommandLine[SHIM_CMDLINE_SIZE];
} shim_boot_args;
#endif
_Static_assert(sizeof(shim_boot_args) == 312, "XNU ARM boot_args size");
_Static_assert(offsetof(shim_boot_args, deviceTreeP) == 48, "DT ABI");
_Static_assert(offsetof(shim_boot_args, CommandLine) == 56, "command ABI");
struct xnu_layout { uint32_t args_pa, dt_pa, ttb_pa, reserved_end; };
struct shim_framebuffer { uint32_t base, stride, width, height, depth; };
const char *xnu_set_framebuffer(shim_boot_args *, void *tree, const struct shim_policy *, const struct shim_framebuffer *);
struct shim_identity { const char *model, *compatible; uint32_t uart; };
/* Core functions never access physical memory or MMIO. All sizes are bounded. */
bool shim_range_valid(struct shim_range r);
bool shim_overlap(struct shim_range a, struct shim_range b);
bool shim_hex(const char *s, uint32_t *value);
const char *macho_plan(const void *image, size_t size, const struct shim_policy *, struct macho_plan *);
/* Call only after planning; callback maps a fully validated destination range. */
typedef void *(*shim_map_fn)(void *context, uint32_t address, uint32_t size);
bool macho_load(const void *, const struct macho_plan *, shim_map_fn, void *);
const char *xnu_layout(const struct macho_plan *, const struct shim_policy *, struct xnu_layout *);
const char *xnu_build_args(shim_boot_args *, const struct shim_policy *, const struct xnu_layout *, uint32_t dt_size, const char *command);
const char *xnu_prepare_tree(void *, size_t, size_t *, const void *, size_t, const struct shim_identity *, const struct shim_policy *, const struct macho_plan *, const struct xnu_layout *);
bool shim_dt_compatible(const unsigned char *, uint32_t, const char *);
void *memcpy(void *, const void *, size_t);
void *memset(void *, int, size_t);
size_t shim_strlen(const char *, size_t);
/* Platform ports implement these, with no Mach-O or boot_args parsing. */
uint32_t platform_ram_base(void);
uint32_t platform_ram_limit(void);
uint32_t platform_virtual_base(void);
struct shim_identity platform_identity(void);
void platform_putc(char);
const char *platform_prepare(bool keep_display);
void platform_drain_uart(void);
uint32_t platform_outer_cache_base(void);
int armv7_normalize(uint32_t outer_cache_base);
bool armv7_identity_range(uint32_t base, uint32_t size);
void armv7_enter(uint32_t entry, uint32_t boot_args) __attribute__((noreturn));
void shim_halt(void) __attribute__((noreturn));
#endif
