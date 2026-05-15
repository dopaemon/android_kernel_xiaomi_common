// SPDX-License-Identifier: GPL-2.0-only
#define pr_fmt(fmt) "dora_sched: " fmt

#include <linux/jiffies.h>
#include <linux/printk.h>
#include <linux/sched.h>
#include <linux/sched/cpufreq.h>
#include <linux/sched/deadline.h>
#include <linux/sched/rt.h>
#include <linux/sched/topology.h>
#include <trace/hooks/sched.h>

#include "dora.h"

unsigned int sysctl_sched_dora_enable = 1;
unsigned int sysctl_sched_dora_mode = 2;
unsigned int sysctl_sched_dora_input_ms = 64;
unsigned int sysctl_sched_dora_wake_ms = 16;
unsigned int sysctl_sched_dora_uclamp_min = 512;
unsigned int sysctl_sched_dora_prefer_big = 1;
unsigned int sysctl_sched_dora_freq_floor[WALT_NR_CPUS] = {
	1209600, 1209600, 1209600, 1209600,
	1574400, 1574400, 1574400, 1766400,
};
unsigned int sysctl_sched_dora_debug;
unsigned int sysctl_sched_dora_reset_stats;
unsigned long sysctl_sched_dora_stats[4];

static DEFINE_PER_CPU(unsigned long, dora_boost_expires);
static atomic64_t dora_wake_boosts;
static atomic64_t dora_cpu_redirects;
static atomic64_t dora_util_boosts;
static atomic64_t dora_freq_boosts;

static bool dora_enabled(void)
{
	return READ_ONCE(sysctl_sched_dora_enable) &&
	       READ_ONCE(sysctl_sched_dora_mode) && !walt_disabled;
}

static void dora_reset_stats(void)
{
	atomic64_set(&dora_wake_boosts, 0);
	atomic64_set(&dora_cpu_redirects, 0);
	atomic64_set(&dora_util_boosts, 0);
	atomic64_set(&dora_freq_boosts, 0);
}

void dora_refresh_stats(void)
{
	sysctl_sched_dora_stats[0] = atomic64_read(&dora_wake_boosts);
	sysctl_sched_dora_stats[1] = atomic64_read(&dora_cpu_redirects);
	sysctl_sched_dora_stats[2] = atomic64_read(&dora_util_boosts);
	sysctl_sched_dora_stats[3] = atomic64_read(&dora_freq_boosts);
}

int dora_enable_handler(struct ctl_table *table, int write, void __user *buffer,
				size_t *lenp, loff_t *ppos)
{
	int ret;
	unsigned int old_enable = READ_ONCE(sysctl_sched_dora_enable);

	ret = proc_douintvec_minmax(table, write, buffer, lenp, ppos);
	if (ret || !write)
		return ret;

	if (old_enable != READ_ONCE(sysctl_sched_dora_enable))
		pr_info("%s mode=%u uclamp_min=%u input_ms=%u wake_ms=%u\n",
			READ_ONCE(sysctl_sched_dora_enable) ? "enabled" : "disabled",
			READ_ONCE(sysctl_sched_dora_mode),
			READ_ONCE(sysctl_sched_dora_uclamp_min),
			READ_ONCE(sysctl_sched_dora_input_ms),
			READ_ONCE(sysctl_sched_dora_wake_ms));

	return 0;
}

int dora_stats_handler(struct ctl_table *table, int write, void __user *buffer,
		       size_t *lenp, loff_t *ppos)
{
	dora_refresh_stats();
	return proc_doulongvec_minmax(table, write, buffer, lenp, ppos);
}

int dora_reset_stats_handler(struct ctl_table *table, int write,
			     void __user *buffer, size_t *lenp, loff_t *ppos)
{
	int ret;

	ret = proc_douintvec_minmax(table, write, buffer, lenp, ppos);
	if (ret || !write)
		return ret;

	if (READ_ONCE(sysctl_sched_dora_reset_stats)) {
		dora_reset_stats();
		WRITE_ONCE(sysctl_sched_dora_reset_stats, 0);
		pr_info("stats reset\n");
	}

	return 0;
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

static bool dora_task_interactive(struct task_struct *p)
{
	if (rt_task(p) || dl_task(p))
		return true;

	return walt_low_latency_task(p) || task_rtg_high_prio(p);
}

static int dora_best_cpu(struct task_struct *p, int base_cpu)
{
	unsigned long best_cap = 0;
	unsigned long best_util = ULONG_MAX;
	int best_cpu = base_cpu;
	int cpu;

	for_each_cpu(cpu, p->cpus_ptr) {
		unsigned long cap, util;

		if (!cpu_active(cpu))
			continue;

		cap = capacity_orig_of(cpu);
		util = cpu_util(cpu);
		if (cap > best_cap || (cap == best_cap && util < best_util)) {
			best_cap = cap;
			best_util = util;
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

	cpu = dora_best_cpu(p, *new_cpu);
	if (cpu < 0 || cpu == *new_cpu)
		return;

	if (capacity_orig_of(*new_cpu) >= capacity_orig_of(cpu))
		return;

	*new_cpu = cpu;
	dora_boost_cpu(cpu, dora_active_ms());
	atomic64_inc(&dora_cpu_redirects);
}

static void dora_try_to_wake_up(void *unused, struct task_struct *p)
{
	if (!dora_enabled() || !dora_task_interactive(p))
		return;

	dora_boost_cpu(task_cpu(p), dora_active_ms());
	atomic64_inc(&dora_wake_boosts);
}

unsigned long dora_adjust_util(int cpu, unsigned long util, unsigned long max)
{
	unsigned long floor;

	if (!dora_enabled() || !dora_cpu_boosted(cpu))
		return util;

	floor = min_t(unsigned long, READ_ONCE(sysctl_sched_dora_uclamp_min), max);
	if (util < floor)
		atomic64_inc(&dora_util_boosts);

	return max(util, floor);
}

unsigned int dora_adjust_freq(int cpu, unsigned int freq)
{
	unsigned int floor;

	if (!dora_enabled() || !dora_cpu_boosted(cpu) || cpu >= WALT_NR_CPUS)
		return freq;

	floor = READ_ONCE(sysctl_sched_dora_freq_floor[cpu]);
	if (freq < floor)
		atomic64_inc(&dora_freq_boosts);

	return max(freq, floor);
}

void dora_init(void)
{
	register_trace_android_rvh_select_task_rq_fair(dora_select_task_rq_fair,
							 NULL);
	register_trace_android_rvh_try_to_wake_up(dora_try_to_wake_up, NULL);

	pr_info("initialized enable=%u mode=%u uclamp_min=%u input_ms=%u wake_ms=%u\n",
		READ_ONCE(sysctl_sched_dora_enable),
		READ_ONCE(sysctl_sched_dora_mode),
		READ_ONCE(sysctl_sched_dora_uclamp_min),
		READ_ONCE(sysctl_sched_dora_input_ms),
		READ_ONCE(sysctl_sched_dora_wake_ms));
}
