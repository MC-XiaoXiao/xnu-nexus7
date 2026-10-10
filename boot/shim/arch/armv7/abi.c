/* SPDX-License-Identifier: BSD-2-Clause */
#include "shim.h"
/* LLVM can lower structure initialization/copy to ARM EABI helpers even in
 * freestanding mode. No libc or compiler runtime is required. */
void __aeabi_memcpy(void *d,const void *s,size_t n) { memcpy(d,s,n); }
void __aeabi_memcpy4(void *d,const void *s,size_t n) { memcpy(d,s,n); }
void __aeabi_memcpy8(void *d,const void *s,size_t n) { memcpy(d,s,n); }
void __aeabi_memclr(void *d,size_t n) { memset(d,0,n); }
void __aeabi_memclr4(void *d,size_t n) { memset(d,0,n); }
void __aeabi_memclr8(void *d,size_t n) { memset(d,0,n); }
