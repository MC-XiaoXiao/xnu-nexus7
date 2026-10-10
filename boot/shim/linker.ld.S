/* SPDX-License-Identifier: BSD-2-Clause */
ENTRY(_start)
SECTIONS {
    . = SHIM_LINK_ADDRESS;
    __shim_start = .;
    .text : { KEEP(*(.text.entry)) *(.text*) }
    .rodata : { *(.rodata*) }
    .data : { *(.data*) }
    . = ALIGN(8);
    .bss (NOLOAD) : {
        __bss_start = .;
        *(.bss*) *(COMMON)
        . = ALIGN(8);
        __bss_end = .;
    }
    .stack (NOLOAD) : {
        . = ALIGN(16);
        . += 0x8000;
        __stack_top = .;
    }
    __shim_end = .;
    ASSERT(__shim_end - __shim_start <= 0x20000, "shim exceeds 128 KiB reservation")
    /DISCARD/ : { *(.ARM.exidx*) *(.ARM.extab*) *(.comment) *(.note*) }
}
