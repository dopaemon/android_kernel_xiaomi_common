/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Copyright (C) 2020 Oplus. All rights reserved.
 */

#ifndef _OPLUS_PREFER_SILVER_H_
#define _OPLUS_PREFER_SILVER_H_

#include <linux/sched.h>

extern int sysctl_prefer_silver;
extern int sysctl_heavy_task_thresh;
extern int sysctl_cpu_util_thresh;
extern int sysctl_freq_ratio_thresh;

extern bool prefer_silver_check_freq(int cpu);
extern bool prefer_silver_check_task_util(struct task_struct *p);
extern bool prefer_silver_check_cpu_util(int cpu);
extern int  find_best_silver_cpu(struct task_struct *p);
extern unsigned long ps_cpu_util(int cpu);

#endif /* _OPLUS_PREFER_SILVER_H_ */
