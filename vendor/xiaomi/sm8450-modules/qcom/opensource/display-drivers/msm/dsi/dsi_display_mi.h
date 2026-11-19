/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Copyright (C) 2026 The LineageOS Project
 */

#ifndef _DSI_DISPLAY_MI_H_
#define _DSI_DISPLAY_MI_H_

#include <linux/kthread.h>
#include <linux/mutex.h>
#include <linux/types.h>

struct dsi_display;

enum dsi_display_mi_state {
	DSI_DISPLAY_MI_POWER,
	DSI_DISPLAY_MI_FPS,
	DSI_DISPLAY_MI_DBV,
	DSI_DISPLAY_MI_STATE_MAX,
};

/**
 * struct dsi_display_mi - Xiaomi specific display state
 * @worker:       Worker sending local HBM commands.
 * @lhbm_work:    Applies @lhbm_target to the panel.
 * @lock:         Protects the fields below.
 * @lhbm_target:  Requested local HBM brightness in nits, 0 if off.
 * @allowed:      Panel is on and may receive local HBM commands.
 * @fod_ui:       Local HBM is visible on the panel.
 * @state:        Values of the pollable state nodes.
 */
struct dsi_display_mi {
	struct kthread_worker *worker;
	struct kthread_work lhbm_work;

	struct mutex lock;
	u32 lhbm_target;
	bool allowed;
	bool fod_ui;
	u32 state[DSI_DISPLAY_MI_STATE_MAX];
};

void dsi_display_mi_init(struct dsi_display *display);

void dsi_display_mi_deinit(struct dsi_display *display);

void dsi_display_mi_set_allowed(struct dsi_display *display, bool allowed);

void dsi_display_mi_notify(struct dsi_display *display,
		enum dsi_display_mi_state state, u32 value);

#endif /* _DSI_DISPLAY_MI_H_ */
