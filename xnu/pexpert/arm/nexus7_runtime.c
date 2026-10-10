/* SPDX-License-Identifier: BSD-2-Clause
 * Modern Clang emits this hook even with -fapple-kext. This minimal target
 * links all code into one permanent kernel image and has no loadable modules.
 * Static kernel objects live until reset; their exit destructors are unused.
 */
#if defined(BOARD_CONFIG_TEGRA30_NEXUS7)
int __cxa_atexit(void (*destructor)(void *), void *argument, void *dso)
{
    (void)destructor;
    (void)argument;
    (void)dso;
    return 0;
}
#endif
