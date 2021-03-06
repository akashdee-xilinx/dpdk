/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2020 Intel Corporation
 */

#include <rte_common.h>
#include <rte_lcore.h>
#include <rte_spinlock.h>

#include "rte_power_intrinsics.h"

/*
 * Per-lcore structure holding current status of C0.2 sleeps.
 */
static struct power_wait_status {
	rte_spinlock_t lock;
	volatile void *monitor_addr; /**< NULL if not currently sleeping */
} __rte_cache_aligned wait_status[RTE_MAX_LCORE];

static inline void
__umwait_wakeup(volatile void *addr)
{
	uint64_t val;

	/* trigger a write but don't change the value */
	val = __atomic_load_n((volatile uint64_t *)addr, __ATOMIC_RELAXED);
	__atomic_compare_exchange_n((volatile uint64_t *)addr, &val, val, 0,
			__ATOMIC_RELAXED, __ATOMIC_RELAXED);
}

static bool wait_supported;

static inline uint64_t
__get_umwait_val(const volatile void *p, const uint8_t sz)
{
	switch (sz) {
	case sizeof(uint8_t):
		return *(const volatile uint8_t *)p;
	case sizeof(uint16_t):
		return *(const volatile uint16_t *)p;
	case sizeof(uint32_t):
		return *(const volatile uint32_t *)p;
	case sizeof(uint64_t):
		return *(const volatile uint64_t *)p;
	default:
		/* shouldn't happen */
		RTE_ASSERT(0);
		return 0;
	}
}

static inline int
__check_val_size(const uint8_t sz)
{
	switch (sz) {
	case sizeof(uint8_t):  /* fall-through */
	case sizeof(uint16_t): /* fall-through */
	case sizeof(uint32_t): /* fall-through */
	case sizeof(uint64_t): /* fall-through */
		return 0;
	default:
		/* unexpected size */
		return -1;
	}
}

/**
 * This function uses UMONITOR/UMWAIT instructions and will enter C0.2 state.
 * For more information about usage of these instructions, please refer to
 * Intel(R) 64 and IA-32 Architectures Software Developer's Manual.
 */
int
rte_power_monitor(const struct rte_power_monitor_cond *pmc,
		const uint64_t tsc_timestamp)
{
	const uint32_t tsc_l = (uint32_t)tsc_timestamp;
	const uint32_t tsc_h = (uint32_t)(tsc_timestamp >> 32);
	const unsigned int lcore_id = rte_lcore_id();
	struct power_wait_status *s;

	/* prevent user from running this instruction if it's not supported */
	if (!wait_supported)
		return -ENOTSUP;

	/* prevent non-EAL thread from using this API */
	if (lcore_id >= RTE_MAX_LCORE)
		return -EINVAL;

	if (pmc == NULL)
		return -EINVAL;

	if (__check_val_size(pmc->data_sz) < 0)
		return -EINVAL;

	s = &wait_status[lcore_id];

	/* update sleep address */
	rte_spinlock_lock(&s->lock);
	s->monitor_addr = pmc->addr;

	/*
	 * we're using raw byte codes for now as only the newest compiler
	 * versions support this instruction natively.
	 */

	/* set address for UMONITOR */
	asm volatile(".byte 0xf3, 0x0f, 0xae, 0xf7;"
			:
			: "D"(pmc->addr));

	/* now that we've put this address into monitor, we can unlock */
	rte_spinlock_unlock(&s->lock);

	/* if we have a comparison mask, we might not need to sleep at all */
	if (pmc->mask) {
		const uint64_t cur_value = __get_umwait_val(
				pmc->addr, pmc->data_sz);
		const uint64_t masked = cur_value & pmc->mask;

		/* if the masked value is already matching, abort */
		if (masked == pmc->val)
			goto end;
	}

	/* execute UMWAIT */
	asm volatile(".byte 0xf2, 0x0f, 0xae, 0xf7;"
			: /* ignore rflags */
			: "D"(0), /* enter C0.2 */
			  "a"(tsc_l), "d"(tsc_h));

end:
	/* erase sleep address */
	rte_spinlock_lock(&s->lock);
	s->monitor_addr = NULL;
	rte_spinlock_unlock(&s->lock);

	return 0;
}

/**
 * This function uses TPAUSE instruction  and will enter C0.2 state. For more
 * information about usage of this instruction, please refer to Intel(R) 64 and
 * IA-32 Architectures Software Developer's Manual.
 */
int
rte_power_pause(const uint64_t tsc_timestamp)
{
	const uint32_t tsc_l = (uint32_t)tsc_timestamp;
	const uint32_t tsc_h = (uint32_t)(tsc_timestamp >> 32);

	/* prevent user from running this instruction if it's not supported */
	if (!wait_supported)
		return -ENOTSUP;

	/* execute TPAUSE */
	asm volatile(".byte 0x66, 0x0f, 0xae, 0xf7;"
			: /* ignore rflags */
			: "D"(0), /* enter C0.2 */
			"a"(tsc_l), "d"(tsc_h));

	return 0;
}

RTE_INIT(rte_power_intrinsics_init) {
	struct rte_cpu_intrinsics i;

	rte_cpu_get_intrinsics_support(&i);

	if (i.power_monitor && i.power_pause)
		wait_supported = 1;
}

int
rte_power_monitor_wakeup(const unsigned int lcore_id)
{
	struct power_wait_status *s;

	/* prevent user from running this instruction if it's not supported */
	if (!wait_supported)
		return -ENOTSUP;

	/* prevent buffer overrun */
	if (lcore_id >= RTE_MAX_LCORE)
		return -EINVAL;

	s = &wait_status[lcore_id];

	/*
	 * There is a race condition between sleep, wakeup and locking, but we
	 * don't need to handle it.
	 *
	 * Possible situations:
	 *
	 * 1. T1 locks, sets address, unlocks
	 * 2. T2 locks, triggers wakeup, unlocks
	 * 3. T1 sleeps
	 *
	 * In this case, because T1 has already set the address for monitoring,
	 * we will wake up immediately even if T2 triggers wakeup before T1
	 * goes to sleep.
	 *
	 * 1. T1 locks, sets address, unlocks, goes to sleep, and wakes up
	 * 2. T2 locks, triggers wakeup, and unlocks
	 * 3. T1 locks, erases address, and unlocks
	 *
	 * In this case, since we've already woken up, the "wakeup" was
	 * unneeded, and since T1 is still waiting on T2 releasing the lock, the
	 * wakeup address is still valid so it's perfectly safe to write it.
	 */
	rte_spinlock_lock(&s->lock);
	if (s->monitor_addr != NULL)
		__umwait_wakeup(s->monitor_addr);
	rte_spinlock_unlock(&s->lock);

	return 0;
}
