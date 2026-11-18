#ifndef _KERNEL_SCHED_BORE_H
#define _KERNEL_SCHED_BORE_H

#include <linux/sched.h>
#include <linux/sched/cputime.h>
#include <linux/atomic.h>
#include <linux/list.h>
#include <linux/rcupdate.h>
#include <linux/jump_label.h>

#define SCHED_BORE_AUTHOR   "Masahito Suzuki"
#define SCHED_BORE_PROGNAME "BORE CPU Scheduler modification"

#define SCHED_BORE_VERSION  "7.0.1"

#ifdef CONFIG_SCHED_BORE
extern u8   __read_mostly sched_bore;
DECLARE_STATIC_KEY_TRUE(sched_bore_key);
extern u8   __read_mostly sched_burst_inherit_type;
extern u8   __read_mostly sched_burst_smoothness;
extern u8   __read_mostly sched_burst_penalty_offset;
extern uint __read_mostly sched_burst_penalty_scale;
extern uint __read_mostly sched_burst_cache_lifetime;
extern u8   __read_mostly sched_burst_protect_slice_lv;

extern uint __read_mostly sched_credit_cap_us;
DECLARE_STATIC_KEY_FALSE(sched_credit_key);

extern void bore_note_sleep(struct task_struct *p, u64 now);
extern u64  bore_credit_ns(struct task_struct *p);

DECLARE_STATIC_KEY_TRUE(sched_burst_protect_slice_cond_key);
DECLARE_STATIC_KEY_FALSE(sched_burst_protect_slice_prefer_key);

static inline u8 bore_score(struct task_struct *p)
{ return p->se.bore.penalty >> 8; }

extern u8   effective_prio_bore(struct task_struct *p);
extern void update_curr_bore(struct task_struct *p, u64 delta_exec);
extern void restart_burst_bore(struct task_struct *p);
extern void restart_burst_rescale_deadline_bore(struct task_struct *p);
extern void task_fork_bore(struct task_struct *p, struct task_struct *parent,
													u64 clone_flags, u64 now);
extern void sched_init_bore(void);
extern void reset_task_bore(struct task_struct *p);

extern int  sched_bore_update_handler(struct ctl_table *table,
	int write, void *buffer, size_t *lenp, loff_t *ppos);
extern int  sched_burst_inherit_type_update_handler(struct ctl_table *table,
	int write, void *buffer, size_t *lenp, loff_t *ppos);
extern int  sched_burst_protect_slice_lv_update_handler(struct ctl_table *table,
	int write, void *buffer, size_t *lenp, loff_t *ppos);
extern int  sched_credit_cap_us_update_handler(struct ctl_table *table,
	int write, void *buffer, size_t *lenp, loff_t *ppos);

extern void reweight_entity(
	struct cfs_rq *cfs_rq, struct sched_entity *se, unsigned long weight);
#endif /* CONFIG_SCHED_BORE */

#endif /* _KERNEL_SCHED_BORE_H */
