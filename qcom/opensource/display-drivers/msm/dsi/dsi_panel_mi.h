/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Copyright (C) 2026 The LineageOS Project
 */

#ifndef _DSI_PANEL_MI_H_
#define _DSI_PANEL_MI_H_

#include <linux/ktime.h>
#include <linux/types.h>
#include <linux/workqueue.h>

#include "dsi_defs.h"

struct dsi_panel;
struct dsi_panel_cmd_set;
struct dsi_parser_utils;
struct dsi_display_mode;

enum dsi_panel_mi_doze {
	DSI_PANEL_MI_DOZE_OFF,
	DSI_PANEL_MI_DOZE_LBM,
	DSI_PANEL_MI_DOZE_HBM,
};

/**
 * struct dsi_panel_mi - Xiaomi specific panel state
 * @hbm_reg53:    The upper half of the backlight range is sent in HBM mode,
 *                which is selected through register 0x53.
 * @hbm_active:   Panel is currently in HBM mode.
 * @last_bl_lvl:  Last backlight level requested by the display driver.
 * @doze_nolp:    Leave doze through the doze HBM/LBM specific NOLP commands.
 * @doze_hbm_threshold: Doze backlight levels above this select doze HBM.
 * @doze:         Panel is in a low power mode.
 * @doze_active:  Doze brightness mode currently set on the panel.
 * @doze_hbm_dbv: DBV set by the doze HBM commands.
 * @doze_lbm_dbv: DBV set by the doze LBM commands.
 * @lhbm:         Panel supports local HBM.
 * @lhbm_ui_ready_frames: Frames until local HBM is visible after enabling it.
 * @lhbm_alpha:   Local HBM alpha (register 0x87) per backlight level.
 * @lhbm_alpha_size: Number of entries in @lhbm_alpha.
 * @lhbm_alpha_max: Alpha for backlight levels beyond @lhbm_alpha.
 * @lhbm_alpha_or: Bits always set in the alpha parameter.
 * @lhbm_df_min:  Minimum of register 0xDF, which is 4 times the DBV.
 * @lhbm_b2_min:  Minimum of register 0xB2, which is the DBV.
 * @lhbm_nits:    Active local HBM brightness, 0 if off.
 * @lhbm_white:   Calibrated white gamma (register 0xD0).
 * @lhbm_white_valid: @lhbm_white was provided by the bootloader.
 * @esd_irq:      Interrupt of the panel error flag, 0 if not available.
 * @esd_irq_flags: Trigger flags of @esd_irq.
 * @esd_irq_enabled: @esd_irq is enabled.
 * @esd_ignore_in_doze: Ignore panel errors while in doze.
 * @dbv_skip:     Range of DBVs the panel can't display, mapped to the DBV below.
 * @bl_sync_threshold: DBVs above this need the BL sync commands, 0 if unused.
 * @bl_sync:      BL sync is enabled.
 * @power_off_delay_ms: Delays after disabling the panel enable GPIO and
 *                after resetting the panel while powering off.
 * @last_fps:     Refresh rate before the current timing switch.
 * @gamma_updated: Gamma was updated after the first timing switch.
 * @gamma_pending: Gamma needs to be updated with the next backlight change.
 * @switch_time:  Time of the last timing switch.
 * @switch_wait_vblank: Wait for a vblank after timing switches.
 * @post_unprepare_delay_ms: Delay before powering off the panel.
 * @batch_doze_lbm: Batches below [0] need 0x53 set to [1] in doze LBM.
 * @batch:        Panel batch (register 0xDA), -1 if not read yet.
 * @doze_cmd_fps: Refresh rate the doze commands are made for, 0 for any.
 * @doze_fallback_dbv: HBM and LBM doze DBVs written at other refresh rates.
 * @dimming_delay_ms: Delay until DDIC dimming is enabled after the panel
 *                turned on or local HBM turned off.
 * @dimming_work: Enables DDIC dimming.
 * @bl_on:        Backlight was turned on since the panel was enabled.
 * @vdc_param:    VDC calibration from the bootloader.
 * @vdc_update:   <register index length> triples of the panel on commands
 *                receiving @vdc_param in order.
 * @vdc_update_count: Number of u32 in @vdc_update.
 * @underflow_black: Show black instead of blue on video mode underflows.
 * @dfps_vfp:     <fps vfp> pairs overriding the dynamic fps front porches.
 * @dfps_vfp_count: Number of u32 in @dfps_vfp.
 */
struct dsi_panel_mi {
	bool hbm_reg53;
	bool hbm_active;
	u32 last_bl_lvl;

	bool doze_nolp;
	u32 doze_hbm_threshold;
	bool doze;
	enum dsi_panel_mi_doze doze_active;
	u32 doze_hbm_dbv;
	u32 doze_lbm_dbv;

	bool lhbm;
	u32 lhbm_ui_ready_frames;
	const u16 *lhbm_alpha;
	u32 lhbm_alpha_size;
	u32 lhbm_alpha_max;
	u32 lhbm_alpha_or;
	u32 lhbm_df_min;
	u32 lhbm_b2_min;
	u32 lhbm_nits;
	u8 lhbm_white[6];
	bool lhbm_white_valid;

	int esd_irq;
	u32 esd_irq_flags;
	bool esd_irq_enabled;
	bool esd_ignore_in_doze;

	u32 dbv_skip[2];
	u32 bl_sync_threshold;
	bool bl_sync;

	u32 power_off_delay_ms[2];

	u32 last_fps;
	bool gamma_updated;
	bool gamma_pending;
	ktime_t switch_time;

	bool switch_wait_vblank;
	u32 post_unprepare_delay_ms;

	u32 batch_doze_lbm[2];
	int batch;

	u32 doze_cmd_fps;
	u32 doze_fallback_dbv[2];

	u32 dimming_delay_ms;
	struct delayed_work dimming_work;
	bool bl_on;

	u8 vdc_param[8];
	u32 vdc_update[6];
	int vdc_update_count;

	bool underflow_black;
	u32 dfps_vfp[8];
	int dfps_vfp_count;
};

void dsi_panel_mi_parse(struct dsi_panel *panel);

void dsi_panel_mi_deinit(struct dsi_panel *panel);

void dsi_panel_mi_parse_cmd_update(struct dsi_panel_cmd_set *set,
		const char *name, struct dsi_parser_utils *utils);

int dsi_panel_mi_update_cmd_set(struct dsi_panel *panel,
		enum dsi_cmd_set_type type, u8 reg, const u8 *val, size_t len);

bool dsi_panel_mi_set_backlight(struct dsi_panel *panel, u32 *bl_lvl);

void dsi_panel_mi_leave_hbm(struct dsi_panel *panel);

void dsi_panel_mi_reset_state(struct dsi_panel *panel);

bool dsi_panel_mi_has_doze(struct dsi_panel *panel);

int dsi_panel_mi_set_doze(struct dsi_panel *panel, bool doze);

int dsi_panel_mi_set_lhbm(struct dsi_panel *panel, u32 nits);

void dsi_panel_mi_esd_irq_ctrl(struct dsi_panel *panel, bool enable);

enum dsi_cmd_set_type dsi_panel_mi_timing_switch_type(struct dsi_panel *panel);

void dsi_panel_mi_post_timing_switch(struct dsi_panel *panel);

void dsi_panel_mi_read_batch(struct dsi_panel *panel);

void dsi_panel_mi_update_on_cmds(struct dsi_panel *panel);

bool dsi_panel_mi_has_fps_gamma(struct dsi_panel *panel);

void dsi_panel_mi_fps_gamma(struct dsi_panel *panel);

void dsi_panel_mi_override_dfps_vfp(struct dsi_panel *panel,
		struct dsi_display_mode *mode);

#endif /* _DSI_PANEL_MI_H_ */
