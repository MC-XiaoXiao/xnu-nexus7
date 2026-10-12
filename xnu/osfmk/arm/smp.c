/* SPDX-License-Identifier: BSD-2-Clause */
/* ARM IOCPU/Mach glue, following the iOS 6 registration and slave_main path.
 * This small implementation deliberately leaves DVFS/cluster switching out. */
#include <mach/mach_types.h>
#include <arm/cpu_data.h>
#include <arm/arch.h>
#include <arm/mp.h>
#include <arm/pmap.h>
#include <arm/pmap_asid.h>
#include <arm/cpufunc.h>
#include <arm/misc_protos.h>
#include <arm/machine_routines.h>
#include <kern/processor.h>
#include <kern/startup.h>
#include <kern/ast.h>
#include <kern/kalloc.h>
#include <vm/vm_kern.h>
#include <pexpert/pexpert.h>

int arm_cpu_logical[MAX_CPUS] = {0, [1 ... MAX_CPUS-1] = -1};
uint32_t arm_secondary_ttbr, arm_secondary_sctlr;
unsigned arm_smp_user_mask, arm_smp_ipi_mask, arm_smp_tick_mask;
unsigned arm_smp_physical_mask;
void arm_smp_tick(arm_saved_state_t *state)
{
    unsigned cpu = get_cpu_number();
    __sync_fetch_and_or(&arm_smp_physical_mask, 1U << current_cpu_datap()->cpu_phys_number);
    unsigned bit = 1U << cpu;
    __sync_fetch_and_or(&arm_smp_tick_mask, bit);
    if ((state->cpsr & 0x1f) == 0x10) __sync_fetch_and_or(&arm_smp_user_mask, bit);
}
static lck_mtx_t *registration_lock;
static lck_grp_t *smp_group;

static void arm_ipi_handler(void)
{
    current_cpu_datap()->ipi_cnt++;
    __sync_fetch_and_or(&arm_smp_ipi_mask, 1U << get_cpu_number());
    ast_check(current_processor());
}

kern_return_t arm_processor_register(cpu_id_t id, unsigned logical, unsigned physical, vm_offset_t reset_entry,
                                     processor_t *out, ipi_handler_t *ipi)
{
    cpu_data_t *data;
    processor_t processor;
    vm_offset_t stack;
    if (!id || !out || !ipi || logical >= MAX_CPUS || physical >= MAX_CPUS || (logical && !reset_entry))
        return KERN_INVALID_ARGUMENT;
    /* CPU0 is registered before any secondary IOCPU is published. */
    if (!registration_lock) {
        smp_group = lck_grp_alloc_init("arm cpu registration", LCK_GRP_ATTR_NULL);
        registration_lock = lck_mtx_alloc_init(smp_group, LCK_ATTR_NULL);
    }
    lck_mtx_lock(registration_lock);
    if (logical == 0) {
        data = &cpu_data_master;
        if (data->cpu_id && data->cpu_id != id) goto fail;
    } else {
        if (logical != real_ncpus || cpu_datap(logical)) goto fail;
        data = kalloc(sizeof(*data));
        processor = cpu_processor_alloc(FALSE);
        if (!data || !processor) panic("ARM CPU allocation failed");
        bzero(data, sizeof(*data));
        if (kmem_alloc(kernel_map, &stack, INTSTACK_SIZE) != KERN_SUCCESS)
            panic("ARM CPU interrupt stack allocation failed");
        data->cpu_this = data;
        data->cpu_number = logical;
        data->cpu_phys_number = physical;
        data->cpu_int_stack_top = stack + INTSTACK_SIZE;
        data->cpu_processor = processor;
        data->cpu_type = cpu_data_master.cpu_type;
        data->cpu_subtype = cpu_data_master.cpu_subtype;
        cpu_data_ptr[logical] = data;
        arm_cpu_logical[physical] = logical;
        processor_init(processor, logical, processor_pset(master_processor));
        ++real_ncpus;
    }
    data->cpu_id = id;
    data->cpu_reset_handler = reset_entry;
    *out = data->cpu_processor;
    *ipi = arm_ipi_handler;
    lck_mtx_unlock(registration_lock);
    return KERN_SUCCESS;
fail:
    lck_mtx_unlock(registration_lock);
    return KERN_FAILURE;
}

unsigned arm_smp_timer_mask(void)
{
    unsigned mask = 0;
    for (unsigned i = 1; i < real_ncpus; ++i)
        if (cpu_datap(i)->cpu_running) mask |= 1U << cpu_datap(i)->cpu_phys_number;
    return mask;
}

kern_return_t arm_cpu_start(int cpu)
{
    if ((unsigned)cpu >= real_ncpus || !cpu_datap(cpu)) return KERN_INVALID_ARGUMENT;
    if (!cpu_datap(cpu)->cpu_reset_handler) return KERN_FAILURE;
    extern uint32_t arm_secondary_ttbr, arm_secondary_sctlr;
    arm_secondary_ttbr = kernel_pmap->pm_l1_phys;
    __asm__ volatile("mrc p15, 0, %0, c1, c0, 0" : "=r"(arm_secondary_sctlr));
    arm_dcache_wbinv_all();
    kern_return_t result = PE_cpu_start(cpu_datap(cpu)->cpu_id, cpu_datap(cpu)->cpu_reset_handler, 0);
    if (result != KERN_SUCCESS)
        kprintf("[smp] physical=%u platform start failed=%d\n", cpu_datap(cpu)->cpu_phys_number, result);
    if (result != KERN_SUCCESS) return result;
    uint64_t deadline = mach_absolute_time() + 1000000000ULL;
    while (!cpu_datap(cpu)->cpu_running) {
#if defined(_ARM_ARCH_7)
        __asm__ volatile("dmb ish" ::: "memory");
#else
        arm_drain_writebuf();
#endif
        if (mach_absolute_time() > deadline) panic("ARM CPU%d startup timeout", cpu);
    }
    return KERN_SUCCESS;
}

void arm_secondary_init(void)
{
    cpu_data_t *data = current_cpu_datap();
    thread_t thread = data->cpu_processor->next_thread;
    if (!thread) thread = data->cpu_processor->idle_thread;
    machine_set_current_thread(thread);
    cpu_init();
    pmap_asid_configure();
    slave_main(NULL);
    panic("ARM slave_main returned");
}
