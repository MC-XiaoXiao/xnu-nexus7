/* SPDX-License-Identifier: BSD-2-Clause */
#include <sys/types.h>
#include <sys/sysctl.h>
extern unsigned arm_smp_user_mask, arm_smp_ipi_mask, arm_smp_tick_mask;
SYSCTL_UINT(_debug, OID_AUTO, arm_smp_user_mask, CTLFLAG_RD | CTLFLAG_LOCKED,
            &arm_smp_user_mask, 0, "Logical CPUs sampled executing user code");
SYSCTL_UINT(_debug, OID_AUTO, arm_smp_ipi_mask, CTLFLAG_RD | CTLFLAG_LOCKED,
            &arm_smp_ipi_mask, 0, "Logical CPUs receiving scheduler IPIs");
SYSCTL_UINT(_debug, OID_AUTO, arm_smp_tick_mask, CTLFLAG_RD | CTLFLAG_LOCKED,
            &arm_smp_tick_mask, 0, "Logical CPUs receiving timer events");

extern unsigned arm_smp_physical_mask;
SYSCTL_UINT(_debug, OID_AUTO, arm_smp_physical_mask, CTLFLAG_RD | CTLFLAG_LOCKED,
            &arm_smp_physical_mask, 0, "Physical CPUs receiving timer events");
