/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef _WALT_DORA_H
#define _WALT_DORA_H

#include "walt.h"

extern unsigned int sysctl_sched_dora_enable;
extern unsigned int sysctl_sched_dora_mode;
extern unsigned int sysctl_sched_dora_input_ms;
extern unsigned int sysctl_sched_dora_wake_ms;
extern unsigned int sysctl_sched_dora_uclamp_min;
extern unsigned int sysctl_sched_dora_prefer_big;
extern unsigned int sysctl_sched_dora_freq_floor[WALT_NR_CPUS];
extern unsigned int sysctl_sched_dora_debug;
extern unsigned int sysctl_sched_dora_reset_stats;
extern unsigned long sysctl_sched_dora_stats[4];

void dora_init(void);
void dora_refresh_stats(void);
int dora_enable_handler(struct ctl_table *table, int write, void __user *buffer,
			size_t *lenp, loff_t *ppos);
int dora_stats_handler(struct ctl_table *table, int write, void __user *buffer,
		       size_t *lenp, loff_t *ppos);
int dora_reset_stats_handler(struct ctl_table *table, int write,
			     void __user *buffer, size_t *lenp, loff_t *ppos);
unsigned long dora_adjust_util(int cpu, unsigned long util, unsigned long max);
unsigned int dora_adjust_freq(int cpu, unsigned int freq);

#endif
