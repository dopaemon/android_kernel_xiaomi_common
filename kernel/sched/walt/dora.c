// SPDX-License-Identifier: GPL-2.0-only
#include <linux/jiffies.h>
#include <linux/sched.h>
#include <linux/sched/cpufreq.h>
#include <linux/sched/deadline.h>
#include <linux/sched/rt.h>
#include <linux/sched/topology.h>
#include <trace/hooks/sched.h>

#include "dora.h"

unsigned int sysctl_sched_dora_enable = 1;
unsigned int sysctl_sched_dora_mode = 2;
unsigned int sysctl_sched_dora_input_ms = 120;
unsigned int sysctl_sched_dora_wake_ms = 32;
unsigned int sysctl_sched_dora_uclamp_min = 768;
unsigned int sysctl_sched_dora_prefer_big = 1;
unsigned int sysctl_sched_dora_freq_floor[WALT_NR_CPUS] = {
	1209600, 1209600, 1209600, 1209600,
	1574400, 1574400, 1574400, 1766400,
};
unsigned int sysctl_sched_dora_debug;

static DEFINE_PER_CPU(unsigned long, dora_boost_expires);
static atomic64_t dora_wake_boosts;
static atomic64_t dora_cpu_redirects;

static bool dora_enabled(void)
{
	return READ_ONCE(sysctl_sched_dora_enable) &&
	       READ_ONCE(sysctl_sched_dora_mode) && !walt_disabled;
}

static unsigned int dora_active_ms(void)
{
	unsigned int mode = READ_ONCE(sysctl_sched_dora_mode);

	return mode > 1 ? READ_ONCE(sysctl_sched_dora_input_ms) :
		READ_ONCE(sysctl_sched_dora_wake_ms);
}

static void dora_boost_cpu(int cpu, unsigned int msec)
{
	unsigned long expires;

	if (!cpu_possible(cpu) || !msec)
		return;

	expires = jiffies + msecs_to_jiffies(msec);
	WRITE_ONCE(per_cpu(dora_boost_expires, cpu), expires);
}

static bool dora_cpu_boosted(int cpu)
{
	unsigned long expires;

	if (!cpu_possible(cpu))
		return false;

	expires = READ_ONCE(per_cpu(dora_boost_expires, cpu));
	return time_before(jiffies, expires);
}

static void dora_boost_allowed_cpus(struct task_struct *p, unsigned int msec)
{
	int cpu;

	for_each_cpu(cpu, p->cpus_ptr)
		dora_boost_cpu(cpu, msec);
}

static bool dora_task_interactive(struct task_struct *p)
{
	if (rt_task(p) || dl_task(p))
		return true;

	return task_nice(p) <= 0;
}

static int dora_best_cpu(struct task_struct *p, int prev_cpu)
{
	unsigned long best_cap = 0;
	int best_cpu = prev_cpu;
	int cpu;

	for_each_cpu(cpu, p->cpus_ptr) {
		unsigned long cap;

		if (!cpu_active(cpu))
			continue;

		cap = capacity_orig_of(cpu);
		if (cap >= best_cap) {
			best_cap = cap;
			best_cpu = cpu;
		}
	}

	return best_cpu;
}

static void dora_select_task_rq_fair(void *unused, struct task_struct *p,
					    int prev_cpu, int sd_flag, int wake_flags,
					    int *new_cpu)
{
	int cpu;

	if (!dora_enabled() || !READ_ONCE(sysctl_sched_dora_prefer_big))
		return;

	if (!dora_task_interactive(p))
		return;

	cpu = dora_best_cpu(p, prev_cpu);
	if (cpu < 0 || cpu == *new_cpu)
		return;

	*new_cpu = cpu;
	dora_boost_cpu(cpu, dora_active_ms());
	atomic64_inc(&dora_cpu_redirects);
}

static void dora_try_to_wake_up(void *unused, struct task_struct *p)
{
	if (!dora_enabled() || !dora_task_interactive(p))
		return;

	dora_boost_allowed_cpus(p, dora_active_ms());
	atomic64_inc(&dora_wake_boosts);
}

static void dora_scheduler_tick(void *unused, struct rq *rq)
{
	int cpu = cpu_of(rq);

	if (!dora_enabled())
		return;

	if (rq->nr_running > 1)
		dora_boost_cpu(cpu, READ_ONCE(sysctl_sched_dora_wake_ms));
}

unsigned long dora_adjust_util(int cpu, unsigned long util, unsigned long max)
{
	unsigned long floor;

	if (!dora_enabled() || !dora_cpu_boosted(cpu))
		return util;

	floor = min_t(unsigned long, READ_ONCE(sysctl_sched_dora_uclamp_min), max);
	return max(util, floor);
}

unsigned int dora_adjust_freq(int cpu, unsigned int freq)
{
	unsigned int floor;

	if (!dora_enabled() || !dora_cpu_boosted(cpu) || cpu >= WALT_NR_CPUS)
		return freq;

	floor = READ_ONCE(sysctl_sched_dora_freq_floor[cpu]);
	return max(freq, floor);
}

void dora_init(void)
{
	register_trace_android_rvh_select_task_rq_fair(dora_select_task_rq_fair,
							 NULL);
	register_trace_android_rvh_try_to_wake_up(dora_try_to_wake_up, NULL);
	register_trace_android_vh_scheduler_tick(dora_scheduler_tick, NULL);
}
