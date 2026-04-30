// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (C) 2020 Oplus. All rights reserved.
 */
 
#include <linux/sched.h>
#include <linux/sysctl.h>
#include <linux/reciprocal_div.h>
#include <linux/topology.h>
#include <linux/cpufreq.h>
#include <linux/atomic.h>
#include <linux/prefer_silver.h>
#include "sched.h"

#ifdef CONFIG_DEBUG_FS
#include <linux/debugfs.h>
#include <linux/seq_file.h>
#endif

/* ------------------------------------------------------------------ *
 * Tunables
 * ------------------------------------------------------------------ */
int sysctl_prefer_silver     = 1;
int sysctl_heavy_task_thresh = 75;
int sysctl_cpu_util_thresh   = 90;
int sysctl_freq_ratio_thresh = 95;

/* ------------------------------------------------------------------ *
 * Debug counters
 * ------------------------------------------------------------------ */
static atomic_t ps_hit_count       = ATOMIC_INIT(0);
static atomic_t ps_miss_count      = ATOMIC_INIT(0);
static atomic_t ps_miss_no_silver  = ATOMIC_INIT(0);
static atomic_t ps_miss_freq       = ATOMIC_INIT(0);
static atomic_t ps_miss_util       = ATOMIC_INIT(0);
static atomic_t ps_miss_affinity   = ATOMIC_INIT(0);
static atomic_t ps_miss_task_heavy = ATOMIC_INIT(0);
static atomic_t ps_boot_miss       = ATOMIC_INIT(0);

/* ------------------------------------------------------------------ *
 * Cluster state
 * ------------------------------------------------------------------ */
static unsigned long ps_silver_cap;
static unsigned long ps_gold_cap;
static cpumask_t     ps_silver_mask;
static cpumask_t     ps_silver_online;
static atomic_t      ps_detected = ATOMIC_INIT(0);

static void update_silver_online(void)
{
	cpumask_and(&ps_silver_online, &ps_silver_mask, cpu_online_mask);
}

static inline bool cpu_is_silver(int cpu)
{
	return cpumask_test_cpu(cpu, &ps_silver_mask);
}

static bool detect_cluster_capacity(void)
{
	cpumask_t tmp_silver_mask;
	unsigned long min_cap    = ULONG_MAX;
	unsigned long second_cap = ULONG_MAX;
	int cpu;

	cpumask_clear(&tmp_silver_mask);

#ifdef CONFIG_SCHED_WALT
	if (!list_empty(&cluster_head)) {
		struct sched_cluster *cluster;
		int cluster_count = 0;

		for_each_sched_cluster(cluster)
			cluster_count++;

		if (cluster_count < 2)
			return false;

		for_each_sched_cluster(cluster) {
			int first_cpu = cpumask_first(&cluster->cpus);
			unsigned long cap;

			if (first_cpu >= nr_cpu_ids)
				continue;

			cap = capacity_orig_of(first_cpu);

			if (cap < min_cap) {
				if (min_cap != ULONG_MAX)
					second_cap = min_cap;
				min_cap = cap;
				cpumask_copy(&tmp_silver_mask, &cluster->cpus);
			} else if (cap > (min_cap * 115 / 100) &&
				   cap < second_cap) {
				second_cap = cap;
			}
		}
		goto validate;
	}
#endif

	for_each_possible_cpu(cpu) {
		unsigned long cap = capacity_orig_of(cpu);
		if (cap < min_cap)
			min_cap = cap;
	}
	for_each_possible_cpu(cpu) {
		unsigned long cap = capacity_orig_of(cpu);
		if (cap > (min_cap * 115 / 100) && cap < second_cap)
			second_cap = cap;
	}
	for_each_possible_cpu(cpu) {
		if (capacity_orig_of(cpu) <= (min_cap * 115 / 100))
			cpumask_set_cpu(cpu, &tmp_silver_mask);
	}

#ifdef CONFIG_SCHED_WALT
validate:
#endif
	if (min_cap == ULONG_MAX || cpumask_empty(&tmp_silver_mask))
		return false;

	if (cpumask_weight(&tmp_silver_mask) >= num_possible_cpus())
		return false;

	ps_silver_cap = min_cap;
	ps_gold_cap   = (second_cap == ULONG_MAX) ? (min_cap * 2) : second_cap;
	cpumask_copy(&ps_silver_mask, &tmp_silver_mask);
	update_silver_online();

	if (cpumask_empty(&ps_silver_online))
		cpumask_and(&ps_silver_online, &ps_silver_mask, cpu_possible_mask);

	if (cpumask_empty(&ps_silver_online))
		return false;

	return true;
}

static bool ps_ensure_detected(void)
{
	if (likely(atomic_read(&ps_detected)))
		return !cpumask_empty(&ps_silver_online);

	if (!detect_cluster_capacity()) {
		atomic_inc(&ps_boot_miss);
		return false;
	}

	atomic_set(&ps_hit_count,       0);
	atomic_set(&ps_miss_count,      0);
	atomic_set(&ps_miss_no_silver,  0);
	atomic_set(&ps_miss_affinity,   0);
	atomic_set(&ps_miss_freq,       0);
	atomic_set(&ps_miss_util,       0);
	atomic_set(&ps_miss_task_heavy, 0);

	smp_wmb();
	atomic_set(&ps_detected, 1);

	pr_info("prefer_silver v7.9: ready cap=%lu/%lu mask=%*pbl online=%*pbl boot_miss=%d\n",
		ps_silver_cap, ps_gold_cap,
		cpumask_pr_args(&ps_silver_mask),
		cpumask_pr_args(&ps_silver_online),
		atomic_read(&ps_boot_miss));

	return true;
}

static int ps_cpu_online_cb(unsigned int cpu)
{
	if (!atomic_read(&ps_detected))
		return 0;
	if (cpu_is_silver(cpu))
		cpumask_set_cpu(cpu, &ps_silver_online);
	return 0;
}

static int ps_cpu_offline_cb(unsigned int cpu)
{
	cpumask_clear_cpu(cpu, &ps_silver_online);
	return 0;
}

/* ------------------------------------------------------------------ *
 * Util helpers
 * ------------------------------------------------------------------ */
static inline unsigned long ps_task_util(struct task_struct *p)
{
#ifdef CONFIG_SCHED_WALT
	if (likely(!sched_disable_window_stats))
		return (unsigned long)p->ravg.demand_scaled;
#endif
	return (unsigned long)p->se.avg.util_avg;
}

unsigned long ps_cpu_util(int cpu)
{
#ifdef CONFIG_SCHED_WALT
	if (likely(!sched_disable_window_stats)) {
		unsigned long walt_util =
			(unsigned long)cpu_rq(cpu)->walt_stats.cumulative_runnable_avg_scaled;
		return min_t(unsigned long, walt_util, capacity_orig_of(cpu));
	}
#endif
	return (unsigned long)cpu_rq(cpu)->cfs.avg.util_avg;
}

static inline unsigned long ps_task_util_pct(struct task_struct *p)
{
	unsigned long util = ps_task_util(p);
	unsigned long cap  = ps_silver_cap ? ps_silver_cap
			   : capacity_orig_of(task_cpu(p));

	if (unlikely(!cap))
		return 100;
	return (util * 100) / cap;
}

static inline unsigned long ps_cpu_util_pct(int cpu)
{
	unsigned long cap = capacity_orig_of(cpu);

	if (unlikely(!cap))
		return 100;
	return (ps_cpu_util(cpu) * 100) / cap;
}

/* ------------------------------------------------------------------ *
 * Check functions
 * ------------------------------------------------------------------ */
bool prefer_silver_check_freq(int cpu)
{
	unsigned long silver_freq   = 0;
	unsigned long gold_max_freq = 0;

#ifdef CONFIG_SCHED_WALT
	if (likely(!sched_disable_window_stats)) {
		silver_freq   = (unsigned long)cpu_cur_freq(cpu);
		gold_max_freq = (unsigned long)max_possible_freq;
		goto check;
	}
#endif
	silver_freq   = (unsigned long)cpufreq_quick_get(cpu);
	gold_max_freq = (unsigned long)cpufreq_quick_get_max(cpu);

#ifdef CONFIG_SCHED_WALT
check:
#endif
	if (!silver_freq || !gold_max_freq)
		return true;
	return silver_freq <= (gold_max_freq * sysctl_freq_ratio_thresh / 100);
}

bool prefer_silver_check_cpu_util(int cpu)
{
	return ps_cpu_util_pct(cpu) < sysctl_cpu_util_thresh;
}

bool prefer_silver_check_task_util(struct task_struct *p)
{
	return ps_task_util_pct(p) < sysctl_heavy_task_thresh;
}

/* ------------------------------------------------------------------ *
 * Main placement — v1.0
 * ------------------------------------------------------------------ */
int find_best_silver_cpu(struct task_struct *p)
{
	int i;
	int best_cpu          = -1;
	int best_cpu_fallback = -1;
	unsigned long min_util          = ULONG_MAX;
	unsigned long min_util_fallback = ULONG_MAX;
	unsigned long min_gold_util     = ULONG_MAX;
	bool has_silver    = false;
	bool freq_blocked  = false;
	bool util_blocked  = false;
	bool aff_blocked   = false;
	bool freq_fallback = false;
	unsigned long task_util_pct;
	bool skip_freq_gate;

	if (!sysctl_prefer_silver)
		return -1;

	if (!ps_ensure_detected())
		return -1;

	task_util_pct = ps_task_util_pct(p);

	if (task_util_pct >= sysctl_heavy_task_thresh) {
		atomic_inc(&ps_miss_count);
		atomic_inc(&ps_miss_task_heavy);
		return -1;
	}

	skip_freq_gate = (task_util_pct < 40);

retry:
	for_each_cpu(i, &ps_silver_online) {
		unsigned long cur_util;
		unsigned long weighted_util;

		has_silver = true;

		if (!cpumask_test_cpu(i, p->cpus_ptr)) {
			aff_blocked = true;
			continue;
		}

		if (!skip_freq_gate && !freq_fallback &&
		    !prefer_silver_check_freq(i)) {
			freq_blocked = true;
			continue;
		}

		cur_util = ps_cpu_util(i);

		if (cur_util < min_util_fallback) {
			min_util_fallback = cur_util;
			best_cpu_fallback = i;
		}

		if (!prefer_silver_check_cpu_util(i)) {
			util_blocked = true;
			continue;
		}

		if (cur_util == 0 || available_idle_cpu(i))
			weighted_util = 0;
		else
			weighted_util = cur_util + (ps_silver_cap >> 3);

		if (weighted_util < min_util) {
			min_util = weighted_util;
			best_cpu = i;
		}
	}

	if (best_cpu < 0 && !freq_fallback && !skip_freq_gate &&
	    freq_blocked && !aff_blocked && !util_blocked) {
		freq_fallback = true;
		goto retry;
	}

	if (best_cpu >= 0) {
		atomic_inc(&ps_hit_count);
		return best_cpu;
	}

	if (best_cpu_fallback >= 0 && !aff_blocked) {
		unsigned long g_util;

		for_each_cpu(i, cpu_online_mask) {
			if (cpu_is_silver(i))
				continue;
			if (!cpumask_test_cpu(i, p->cpus_ptr))
				continue;
			g_util = ps_cpu_util(i);
			if (g_util < min_gold_util)
				min_gold_util = g_util;
		}

		if (min_gold_util == ULONG_MAX ||
		    min_util_fallback <= (min_gold_util * 110 / 100)) {
			atomic_inc(&ps_hit_count);
			return best_cpu_fallback;
		}
	}

	atomic_inc(&ps_miss_count);
	if (!has_silver)
		atomic_inc(&ps_miss_no_silver);
	else if (aff_blocked)
		atomic_inc(&ps_miss_affinity);
	else if (freq_blocked)
		atomic_inc(&ps_miss_freq);
	else
		atomic_inc(&ps_miss_util);

	return -1;
}

/* ------------------------------------------------------------------ *
 * Debugfs
 * ------------------------------------------------------------------ */
#ifdef CONFIG_DEBUG_FS

static struct dentry *ps_debugfs_dir;

static int ps_stats_show(struct seq_file *m, void *v)
{
	int hits, misses, total, hit_pct;
	int cpu;
	bool first;

	(void)atomic_read(&ps_detected);
	smp_rmb();

	hits    = atomic_read(&ps_hit_count);
	misses  = atomic_read(&ps_miss_count);
	total   = hits + misses;
	hit_pct = total ? (hits * 100) / total : 0;

	seq_printf(m, "enabled:           %d\n",   sysctl_prefer_silver);
	seq_printf(m, "detected:          %d\n",   atomic_read(&ps_detected));
	seq_printf(m, "boot_miss:         %d\n",   atomic_read(&ps_boot_miss));
	seq_puts(m,   "---\n");
	seq_printf(m, "hits:              %d\n",   hits);
	seq_printf(m, "misses:            %d\n",   misses);
	seq_printf(m, "hit_rate:          %d%%\n", hit_pct);
	seq_puts(m,   "--- miss breakdown ---\n");
	seq_printf(m, "miss_no_silver:    %d\n",   atomic_read(&ps_miss_no_silver));
	seq_printf(m, "miss_affinity:     %d\n",   atomic_read(&ps_miss_affinity));
	seq_printf(m, "miss_freq:         %d\n",   atomic_read(&ps_miss_freq));
	seq_printf(m, "miss_util:         %d\n",   atomic_read(&ps_miss_util));
	seq_printf(m, "miss_task_heavy:   %d\n",   atomic_read(&ps_miss_task_heavy));
	seq_puts(m,   "--- tunables ---\n");
	seq_printf(m, "heavy_task_thresh: %d%%\n", sysctl_heavy_task_thresh);
	seq_printf(m, "cpu_util_thresh:   %d%%\n", sysctl_cpu_util_thresh);
	seq_printf(m, "freq_ratio_thresh: %d%%\n", sysctl_freq_ratio_thresh);
	seq_printf(m, "silver_cap:        %lu\n",  ps_silver_cap);
	seq_printf(m, "gold_cap:          %lu\n",  ps_gold_cap);

	seq_puts(m, "silver_mask:       ");
	first = true;
	for_each_cpu(cpu, &ps_silver_mask) {
		if (!first)
			seq_puts(m, ",");
		seq_printf(m, "%d", cpu);
		first = false;
	}
	if (first)
		seq_puts(m, "(none)");
	seq_puts(m, "\n");

	seq_puts(m, "silver_online:     ");
	first = true;
	for_each_cpu(cpu, &ps_silver_online) {
		if (!first)
			seq_puts(m, ",");
		seq_printf(m, "%d", cpu);
		first = false;
	}
	if (first)
		seq_puts(m, "(none)");
	seq_puts(m, "\n");

	return 0;
}
DEFINE_SHOW_ATTRIBUTE(ps_stats);

static ssize_t ps_reset_write(struct file *f, const char __user *buf,
			      size_t count, loff_t *ppos)
{
	atomic_set(&ps_hit_count,       0);
	atomic_set(&ps_miss_count,      0);
	atomic_set(&ps_miss_no_silver,  0);
	atomic_set(&ps_miss_affinity,   0);
	atomic_set(&ps_miss_freq,       0);
	atomic_set(&ps_miss_util,       0);
	atomic_set(&ps_miss_task_heavy, 0);
	pr_info("prefer_silver: counters reset\n");
	return count;
}

static const struct file_operations ps_reset_fops = {
	.write  = ps_reset_write,
	.llseek = noop_llseek,
};

static void prefer_silver_debugfs_init(void)
{
	ps_debugfs_dir = debugfs_create_dir("prefer_silver", NULL);
	if (IS_ERR_OR_NULL(ps_debugfs_dir)) {
		ps_debugfs_dir = NULL;
		return;
	}
	debugfs_create_file("stats", 0444, ps_debugfs_dir, NULL, &ps_stats_fops);
	debugfs_create_file("reset", 0200, ps_debugfs_dir, NULL, &ps_reset_fops);
}

#else
static inline void prefer_silver_debugfs_init(void) { }
#endif

/* ------------------------------------------------------------------ *
 * Sysctl
 * ------------------------------------------------------------------ */
static struct ctl_table prefer_silver_table[] = {
	{
		.procname     = "sysctl_prefer_silver",
		.data         = &sysctl_prefer_silver,
		.maxlen       = sizeof(int),
		.mode         = 0644,
		.proc_handler = proc_dointvec,
	},
	{
		.procname     = "heavy_task_thresh",
		.data         = &sysctl_heavy_task_thresh,
		.maxlen       = sizeof(int),
		.mode         = 0644,
		.proc_handler = proc_dointvec,
	},
	{
		.procname     = "sysctl_cpu_util_thresh",
		.data         = &sysctl_cpu_util_thresh,
		.maxlen       = sizeof(int),
		.mode         = 0644,
		.proc_handler = proc_dointvec,
	},
	{
		.procname     = "freq_ratio_thresh",
		.data         = &sysctl_freq_ratio_thresh,
		.maxlen       = sizeof(int),
		.mode         = 0644,
		.proc_handler = proc_dointvec,
	},
	{ }
};

static struct ctl_table prefer_silver_root[] = {
	{
		.procname = "kernel",
		.mode     = 0555,
		.child    = prefer_silver_table,
	},
	{ }
};

static struct ctl_table_header *ps_sysctl_header;

/* ------------------------------------------------------------------ *
 * Init
 * ------------------------------------------------------------------ */
int __init prefer_silver_init(void)
{
	int ret;

	cpumask_clear(&ps_silver_mask);
	cpumask_clear(&ps_silver_online);

	ret = cpuhp_setup_state_nocalls(CPUHP_AP_ONLINE_DYN,
					"sched/prefer_silver:online",
					ps_cpu_online_cb,
					ps_cpu_offline_cb);
	if (ret < 0)
		pr_warn("prefer_silver: cpuhp register failed (%d)\n", ret);

	ps_sysctl_header = register_sysctl_table(prefer_silver_root);
	if (!ps_sysctl_header) {
		pr_err("prefer_silver: failed to register sysctl table\n");
		return -ENOMEM;
	}

	prefer_silver_debugfs_init();

	pr_info("prefer_silver v1.0: init OK | enabled=%d\n",
		sysctl_prefer_silver);

	return 0;
}
late_initcall(prefer_silver_init);
