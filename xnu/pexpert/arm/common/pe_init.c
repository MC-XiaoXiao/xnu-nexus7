/*
 * Copyright 2013, winocm. <winocm@icloud.com>
 * All rights reserved.
 * 
 * Redistribution and use in source and binary forms, with or without modification,
 * are permitted provided that the following conditions are met:
 * 
 *   Redistributions of source code must retain the above copyright notice, this
 *   list of conditions and the following disclaimer.
 * 
 *   Redistributions in binary form must reproduce the above copyright notice, this
 *   list of conditions and the following disclaimer in the documentation and/or
 *   other materials provided with the distribution.
 * 
 *   If you are going to use this software in any form that does not involve
 *   releasing the source to this project or improving it, let me know beforehand.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
 * WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
 * DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE FOR
 * ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
 * (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
 * LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON
 * ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
 * SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */
/*
 * Platform expert initialization module.
 */

#include <sys/types.h>
#include <mach/vm_param.h>
#include <machine/machine_routines.h>
#include <pexpert/protos.h>
#include <pexpert/pexpert.h>
#include <pexpert/boot.h>
#include <pexpert/device_tree.h>
#include <pexpert/apple_dt.h>
#include <pexpert/pe_images.h>
#include <kern/sched_prim.h>
#include <kern/debug.h>
#include "boot_images.h"

/* private globals */
PE_state_t PE_state;

char firmware_version[32];
int pe_initialized = 0;

extern uint32_t debug_enabled;
extern void pe_identify_machine(void *args);

boot_args *BootArgs;

int PE_initialize_console( PE_Video * info, int op )
{
    static int   last_console = -1;

    if (info) {
    info->v_offset  = 0;
    info->v_length  = 0;
    info->v_display = GRAPHICS_MODE;
    }

    switch ( op ) {

        case kPEDisableScreen:
            initialize_screen(info, op);
            kprintf("kPEDisableScreen %d\n", last_console);
        if (!console_is_serial())
        last_console = switch_to_serial_console();
            break;

        case kPEEnableScreen:
            initialize_screen(info, op);
            if (info) PE_state.video = *info;
            kprintf("kPEEnableScreen %d\n", last_console);
            if( last_console != -1)
                switch_to_old_console( last_console);
            break;
    
        case kPEBaseAddressChange:
            if (info) PE_state.video = *info;
            /* fall thru */

        default:
            initialize_screen(info, op);
            break;
    }

    return 0;
}

static void null_putc(int c)
{
    return;
}

/**
 * PE_init_platform
 *
 * Initialize the platform expert for ARM.
 */
void PE_init_platform(boolean_t vm_initialized, void *_args)
{
    boot_args *args = (boot_args *) _args;

    if (PE_state.initialized == FALSE) {
        PE_early_puts("PE_init_platform: My name is Macintosh.\n");
        PE_early_puts("PE_init_platform: Initializing for the first time.\n");

        PE_state.initialized = TRUE;
        PE_state.bootArgs = _args;
        PE_state.deviceTreeHead = args->deviceTreeP;

        /* initialize_screen uses bit 0 to distinguish a physical address. */
        PE_state.video.v_baseAddr = args->Video.v_baseAddr ? args->Video.v_baseAddr | 1UL : 0;
        PE_state.video.v_rowBytes = args->Video.v_rowBytes;
        PE_state.video.v_width = args->Video.v_width;
        PE_state.video.v_height = args->Video.v_height;
        PE_state.video.v_depth = args->Video.v_depth;
        PE_state.video.v_display = args->Video.v_display;

        strncpy(PE_state.video.v_pixelFormat, "PPPPPPPP", 8);

        BootArgs = args;
    }

    if (!vm_initialized) {
        /*
         * Initialize the device tree crap. 
         */
        PE_early_puts("PE_init_platform: Initializing device tree\n");
#if defined(BOARD_CONFIG_TEGRA30_NEXUS7)
        /* locore relocates r0, but deviceTreeP is already a kernel VA. Ensure
         * the complete blob lies in loader-reserved RAM before walking it. */
        uint32_t dtva = (uint32_t)(uintptr_t)args->deviceTreeP;
        uint32_t offset = dtva - args->virtBase;
        uint32_t reserved = args->topOfKernelData - args->physBase;
        if (dtva < args->virtBase || (dtva & 0xfff) ||
            args->topOfKernelData < args->physBase || reserved > args->memSize ||
            offset >= reserved || args->deviceTreeLength > reserved-offset ||
            !ADTValid(args->deviceTreeP, args->deviceTreeLength))
            panic("Invalid or unreserved Apple device tree");
        uint32_t map_size;
        const unsigned char *dtmap = ADTProperty(args->deviceTreeP,
            "/chosen/memory-map", "DeviceTree", &map_size);
        if (!dtmap || map_size != 8 || ADTRead(dtmap) != args->physBase+offset ||
            ADTRead(dtmap+4) != args->deviceTreeLength)
            panic("Apple device tree memory-map disagrees with boot_args");
#endif
        DTInit(PE_state.deviceTreeHead);

        PE_early_puts("PE_init_platform: Calling pe_identify_machine\n");
        pe_identify_machine(NULL);
    } else {
        DTEntry entry;
        char *fversion;
        void *map;
        unsigned int size;

        pe_initialized = 1;

        kprintf("PE_init_platform: It sure is great to get out of that bag.\n");
        PE_init_SocSupport();

        /*
         * Reset kputc. 
         */
        PE_kputc = gPESocDispatch.uart_putc;
        unsigned int boot_arg;

#if !defined(BOARD_CONFIG_QSD8250_LEO) && !defined(BOARD_CONFIG_TEGRA30_NEXUS7)
        if (PE_parse_boot_argn("kprintf", &boot_arg, sizeof(boot_arg)))
            PE_kputc = cnputc;
#endif

        if (PE_parse_boot_argn("silence_kprintf", &boot_arg, sizeof(boot_arg)))
            PE_kputc = null_putc;

        /*
         * XXX: Real iOS kernel does iBoot/debug-enabled init after the DTInit call. 
         */
        if (kSuccess == DTLookupEntry(NULL, "/chosen", &entry)) {
            /*
             * What's the iBoot version on this bad boy? 
             */
            if (kSuccess == DTGetProperty(entry, "firmware-version", (void **) &fversion, &size)) {
                unsigned int count = size;
                if (count >= sizeof(firmware_version)) count = sizeof(firmware_version)-1;
                if (fversion && count) ovbcopy(fversion, firmware_version, count);
                firmware_version[count] = 0;
            }
            /*
             * Is the SoC debug-enabled? 
             */
            if (kSuccess == DTGetProperty(entry, "debug-enabled", (void **) &map, &size)) {
                debug_enabled = 0;
                if (size > sizeof(debug_enabled)) size = sizeof(debug_enabled);
                if (map && size) ovbcopy(map, &debug_enabled, size);
            }
        }

        pe_arm_init_interrupts(NULL);
    }
}

/**
 * PE_init_iokit
 *
 * Start IOKit and also set up the video console.
 */
void PE_init_iokit(void)
{
    enum { kMaxBootVar = 128 };

    typedef struct {
        char name[32];
        unsigned long length;
        unsigned long value[2];
    } DriversPackageProp;

    boolean_t bootClutInitialized = FALSE;
    boolean_t norootInitialized = FALSE;

    DTEntry entry;
    unsigned int size;
    uint32_t *map;
    boot_progress_element *bootPict;

    kprintf("Kernel boot args: '%s'\n", PE_boot_args());

    /*
     * Fetch the CLUT and the noroot image.
     */
    if (kSuccess == DTLookupEntry(NULL, "/chosen/memory-map", &entry)) {
        if (kSuccess == DTGetProperty(entry, "BootCLUT", (void **) &map, &size)) {
            if (sizeof(appleClut8) <= map[1]) {
                bcopy((void *) (map[0]), appleClut8, sizeof(appleClut8));
                bootClutInitialized = TRUE;
            }
        }
    }

    if (!bootClutInitialized) {
        bcopy((void *) (uintptr_t) bootClut, (void *) appleClut8, sizeof(appleClut8));
    }

    if (!norootInitialized) {
        default_noroot.width = kFailedBootWidth;
        default_noroot.height = kFailedBootHeight;
        default_noroot.dx = 0;
        default_noroot.dy = kFailedBootOffset;
        default_noroot_data = failedBootPict;
    }

    /*
     * Initialize the panic UI
     */
    panic_ui_initialize((unsigned char *) appleClut8);

    /*
     * Initialize the spinning wheel (progress indicator).
     */
    vc_progress_initialize(&default_progress, default_progress_data1x, default_progress_data2x, (unsigned char *) appleClut8);

    printf("iBoot version: %s\n", firmware_version);

    kprintf("PE_init_iokit: starting IOKit now!\n");

#if defined(BOARD_CONFIG_TEGRA30_NEXUS7)
    gPESocDispatch.framebuffer_init();
#endif
    PE_init_printf(TRUE);

    StartIOKit(PE_state.deviceTreeHead, PE_state.bootArgs, (void *) 0, (void *) 0);
}

/**
 * PE_current_console
 *
 * Get video console information
 */
int PE_current_console(PE_Video * info)
{
    *info = PE_state.video;

    return 0;
}

/* Stub. */
void PE_display_icon(__unused unsigned int flags, __unused const char *name)
{
    if (default_noroot_data)
        vc_display_icon(&default_noroot, default_noroot_data);
}

boolean_t PE_reboot_on_panic(void)
{
    /*
     * Enable reboot-on-panic
     */
    char tempbuf[16];

    if (PE_parse_boot_argn("-panic-reboot", tempbuf, sizeof(tempbuf))) {
        return TRUE;
    } else {
        return FALSE;
    }
}
