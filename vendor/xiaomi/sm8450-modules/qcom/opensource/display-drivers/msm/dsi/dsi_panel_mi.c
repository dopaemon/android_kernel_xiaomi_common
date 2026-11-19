// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (C) 2026 The LineageOS Project
 */

#include <linux/delay.h>
#include <linux/gpio.h>
#include <linux/interrupt.h>
#include <linux/of_gpio.h>
#include <linux/slab.h>
#include <linux/soc/qcom/smem.h>
#include <video/mipi_display.h>

#include "dsi_panel.h"
#include "dsi_panel_mi.h"
#include "dsi_panel_mi_alpha.h"

#define SMEM_SW_DISPLAY_LHBM_TABLE 498
#define SMEM_SW_DISPLAY_VDC_TABLE 501

/*
 * The bootloader stores the calibrated local HBM gamma of Novatek panels in
 * SMEM, as 12 bit R, G and B values. mi,local-hbm-white-param-offset gives the
 * offset of the 1000 nit values, which are optionally scaled by
 * mi,local-hbm-white-scale percent per channel.
 */
static void dsi_panel_mi_parse_lhbm_white(struct dsi_panel *panel)
{
	struct dsi_parser_utils *utils = &panel->utils;
	struct dsi_panel_mi *mi = &panel->mi;
	u32 offset, scale[3], val;
	size_t size;
	u8 *table;
	int i;

	if (utils->read_u32(utils->data, "mi,local-hbm-white-param-offset",
			&offset))
		return;

	table = qcom_smem_get(QCOM_SMEM_HOST_ANY, SMEM_SW_DISPLAY_LHBM_TABLE,
			&size);
	if (IS_ERR_OR_NULL(table) || offset + sizeof(mi->lhbm_white) > size) {
		DSI_INFO("[%s] no local hbm calibration\n", panel->name);
		return;
	}

	memcpy(mi->lhbm_white, &table[offset], sizeof(mi->lhbm_white));
	for (i = 0; i < 3; i++) {
		if (!mi->lhbm_white[i * 2] && !mi->lhbm_white[i * 2 + 1]) {
			DSI_ERR("[%s] invalid local hbm calibration\n",
					panel->name);
			return;
		}
	}

	if (utils->read_u32_array(utils->data, "mi,local-hbm-white-scale",
			scale, 3))
		scale[0] = scale[1] = scale[2] = 100;

	for (i = 0; i < 3; i++) {
		val = (mi->lhbm_white[i * 2] << 8) | mi->lhbm_white[i * 2 + 1];
		val = DIV_ROUND_CLOSEST(val * scale[i], 100);
		mi->lhbm_white[i * 2] = val >> 8;
		mi->lhbm_white[i * 2 + 1] = val & 0xff;
	}

	mi->lhbm_white_valid = true;
}

static void dsi_panel_mi_parse_lhbm(struct dsi_panel *panel)
{
	struct dsi_parser_utils *utils = &panel->utils;
	struct dsi_panel_mi *mi = &panel->mi;
	const char *name;
	int i;

	mi->lhbm = utils->read_bool(utils->data, "mi,local-hbm-enabled");
	if (!mi->lhbm)
		return;

	utils->read_u32(utils->data, "mi,local-hbm-ui-ready-delay-num-frame",
			&mi->lhbm_ui_ready_frames);
	utils->read_u32(utils->data, "mi,local-hbm-df-reg-min",
			&mi->lhbm_df_min);
	utils->read_u32(utils->data, "mi,local-hbm-b2-reg-min",
			&mi->lhbm_b2_min);

	dsi_panel_mi_parse_lhbm_white(panel);

	if (utils->read_string(utils->data, "mi,local-hbm-alpha-table", &name))
		return;

	for (i = 0; i < ARRAY_SIZE(lhbm_alpha_tables); i++) {
		if (strcmp(name, lhbm_alpha_tables[i].name))
			continue;

		mi->lhbm_alpha = lhbm_alpha_tables[i].alpha;
		mi->lhbm_alpha_size = lhbm_alpha_tables[i].size;
		break;
	}

	if (!mi->lhbm_alpha) {
		DSI_ERR("[%s] unknown local hbm alpha table %s\n",
				panel->name, name);
		return;
	}

	mi->lhbm_alpha_max = mi->lhbm_alpha[mi->lhbm_alpha_size - 1];
	utils->read_u32(utils->data, "mi,local-hbm-alpha-max",
			&mi->lhbm_alpha_max);
	utils->read_u32(utils->data, "mi,local-hbm-alpha-or",
			&mi->lhbm_alpha_or);
}

static void dsi_panel_mi_parse_esd(struct dsi_panel *panel)
{
	struct dsi_parser_utils *utils = &panel->utils;
	struct dsi_panel_mi *mi = &panel->mi;
	struct of_phandle_args args;
	u32 flags = 0;
	int gpio, rc;

	gpio = of_get_named_gpio(utils->data, "mi,esd-err-irq-gpio", 0);
	if (!gpio_is_valid(gpio))
		return;

	if (!of_parse_phandle_with_args(utils->data, "mi,esd-err-irq-gpio",
			"#gpio-cells", 0, &args)) {
		if (args.args_count > 1)
			flags = args.args[1];
		of_node_put(args.np);
	}

	rc = devm_gpio_request_one(panel->parent, gpio, GPIOF_IN,
			"esd_err_irq_gpio");
	if (rc) {
		DSI_ERR("[%s] failed to request esd gpio %d, rc=%d\n",
				panel->name, gpio, rc);
		return;
	}

	rc = gpio_to_irq(gpio);
	if (rc < 0)
		return;

	mi->esd_irq = rc;
	mi->esd_irq_flags = flags;
	mi->esd_ignore_in_doze = utils->read_bool(utils->data,
			"mi,panel-ignore-esd-in-aod");
}

static void dsi_panel_mi_update_gamma(struct dsi_panel *panel);

static void dsi_panel_mi_dimming_work(struct work_struct *work)
{
	struct dsi_panel_mi *mi = container_of(to_delayed_work(work),
			struct dsi_panel_mi, dimming_work);
	struct dsi_panel *panel = container_of(mi, struct dsi_panel, mi);

	mutex_lock(&panel->panel_lock);
	/* The HBM, doze and local HBM commands handle dimming themselves */
	if (panel->panel_initialized && !mi->doze && !mi->lhbm_nits &&
	    !mi->hbm_active)
		dsi_panel_tx_cmd_set(panel, DSI_CMD_SET_MI_DIMMINGON);
	mutex_unlock(&panel->panel_lock);
}

static void dsi_panel_mi_queue_dimming(struct dsi_panel *panel)
{
	struct dsi_panel_mi *mi = &panel->mi;

	if (!panel->cur_mode || !panel->cur_mode->priv_info ||
	    !panel->cur_mode->priv_info->cmd_sets[DSI_CMD_SET_MI_DIMMINGON].count)
		return;

	mod_delayed_work(system_wq, &mi->dimming_work,
			msecs_to_jiffies(mi->dimming_delay_ms));
}

static void dsi_panel_mi_parse_vdc(struct dsi_panel *panel)
{
	struct dsi_parser_utils *utils = &panel->utils;
	struct dsi_panel_mi *mi = &panel->mi;
	size_t size;
	u8 *table;
	int count;

	count = utils->count_u32_elems(utils->data, "mi,vdc-param-update");
	if (count <= 0 || count % 3 || count > ARRAY_SIZE(mi->vdc_update))
		return;

	table = qcom_smem_get(QCOM_SMEM_HOST_ANY, SMEM_SW_DISPLAY_VDC_TABLE, &size);
	if (IS_ERR_OR_NULL(table) || size < sizeof(mi->vdc_param)) {
		DSI_INFO("[%s] no vdc calibration\n", panel->name);
		return;
	}

	if (utils->read_u32_array(utils->data, "mi,vdc-param-update",
			mi->vdc_update, count))
		return;

	memcpy(mi->vdc_param, table, sizeof(mi->vdc_param));
	mi->vdc_update_count = count;
}

void dsi_panel_mi_parse(struct dsi_panel *panel)
{
	struct dsi_parser_utils *utils = &panel->utils;
	struct dsi_panel_mi *mi = &panel->mi;

	mi->hbm_reg53 = utils->read_bool(utils->data, "mi,hbm-reg53-enabled");
	mi->doze_nolp = utils->read_bool(utils->data,
			"mi,aod-nolp-command-enabled");

	utils->read_u32(utils->data, "mi,doze-hbm-dbv-level", &mi->doze_hbm_dbv);
	utils->read_u32(utils->data, "mi,doze-lbm-dbv-level", &mi->doze_lbm_dbv);
	if (mi->doze_hbm_dbv && mi->doze_lbm_dbv)
		mi->doze_hbm_threshold = (mi->doze_hbm_dbv + mi->doze_lbm_dbv) / 2;
	else
		mi->doze_hbm_threshold = 100;

	utils->read_u32_array(utils->data, "mi,dbv-skip-range", mi->dbv_skip,
			ARRAY_SIZE(mi->dbv_skip));
	utils->read_u32(utils->data, "mi,bl-sync-threshold",
			&mi->bl_sync_threshold);
	utils->read_u32_array(utils->data, "mi,power-off-delay-ms",
			mi->power_off_delay_ms, ARRAY_SIZE(mi->power_off_delay_ms));
	utils->read_u32(utils->data, "mi,post-unprepare-delay-ms",
			&mi->post_unprepare_delay_ms);
	mi->switch_wait_vblank = utils->read_bool(utils->data,
			"mi,timing-switch-wait-vblank");
	mi->batch = -1;
	utils->read_u32_array(utils->data, "mi,batch-doze-lbm-reg53",
			mi->batch_doze_lbm, ARRAY_SIZE(mi->batch_doze_lbm));
	utils->read_u32(utils->data, "mi,doze-cmd-fps", &mi->doze_cmd_fps);
	utils->read_u32(utils->data, "mi,panel-on-dimming-delay",
			&mi->dimming_delay_ms);
	INIT_DELAYED_WORK(&mi->dimming_work, dsi_panel_mi_dimming_work);
	utils->read_u32_array(utils->data, "mi,doze-fallback-dbv",
			mi->doze_fallback_dbv, ARRAY_SIZE(mi->doze_fallback_dbv));

	dsi_panel_mi_parse_lhbm(panel);
	dsi_panel_mi_parse_esd(panel);
	dsi_panel_mi_parse_vdc(panel);

	mi->underflow_black = utils->read_bool(utils->data,
			"mi,underflow-color-black");
	mi->dfps_vfp_count = utils->count_u32_elems(utils->data, "mi,dfps-vfp");
	if (mi->dfps_vfp_count <= 0 || mi->dfps_vfp_count % 2 ||
	    mi->dfps_vfp_count > ARRAY_SIZE(mi->dfps_vfp) ||
	    utils->read_u32_array(utils->data, "mi,dfps-vfp", mi->dfps_vfp,
			mi->dfps_vfp_count))
		mi->dfps_vfp_count = 0;
}

void dsi_panel_mi_parse_cmd_update(struct dsi_panel_cmd_set *set,
		const char *name, struct dsi_parser_utils *utils)
{
	char prop[64];
	u32 *arr;
	int count, i;

	snprintf(prop, sizeof(prop), "%s-update", name);
	count = utils->count_u32_elems(utils->data, prop);
	if (count <= 0 || count % 3)
		return;

	arr = kcalloc(count, sizeof(*arr), GFP_KERNEL);
	if (!arr)
		return;

	if (utils->read_u32_array(utils->data, prop, arr, count))
		goto exit;

	set->updates = kcalloc(count / 3, sizeof(*set->updates), GFP_KERNEL);
	if (!set->updates)
		goto exit;

	for (i = 0; i < count / 3; i++) {
		set->updates[i].reg = arr[i * 3];
		set->updates[i].index = arr[i * 3 + 1];
		set->updates[i].len = arr[i * 3 + 2];
	}
	set->update_count = count / 3;
exit:
	kfree(arr);
}

int dsi_panel_mi_update_cmd_set(struct dsi_panel *panel,
		enum dsi_cmd_set_type type, u8 reg, const u8 *val, size_t len)
{
	struct dsi_panel_cmd_set *set;
	struct dsi_cmd_update *update;
	struct dsi_cmd_desc *cmd;
	u8 *payload;
	int i, rc = -ENOENT;

	if (!panel->cur_mode || !panel->cur_mode->priv_info)
		return -EINVAL;

	set = &panel->cur_mode->priv_info->cmd_sets[type];
	for (i = 0; i < set->update_count; i++) {
		update = &set->updates[i];
		if (update->reg != reg || update->len != len ||
		    update->index >= set->count)
			continue;

		cmd = &set->cmds[update->index];
		payload = (u8 *)cmd->msg.tx_buf;
		if (cmd->msg.tx_len < len + 1 || payload[0] != reg) {
			DSI_ERR("[%s] invalid update of 0x%02x in set %d\n",
					panel->name, reg, type);
			continue;
		}

		memcpy(&payload[1], val, len);
		rc = 0;
	}

	return rc;
}

static int dsi_panel_mi_tx_with_dbv(struct dsi_panel *panel,
		enum dsi_cmd_set_type type, u32 dbv)
{
	u8 buf[2] = { dbv >> 8, dbv & 0xff };
	int rc;

	rc = dsi_panel_mi_update_cmd_set(panel, type,
			MIPI_DCS_SET_DISPLAY_BRIGHTNESS, buf, sizeof(buf));
	if (rc)
		return rc;

	return dsi_panel_tx_cmd_set(panel, type);
}

/*
 * Returns true if the backlight level was already sent along with an HBM mode
 * switch, otherwise bl_lvl is updated to the DBV for the current mode.
 */
bool dsi_panel_mi_set_backlight(struct dsi_panel *panel, u32 *bl_lvl)
{
	struct dsi_panel_mi *mi = &panel->mi;
	u32 normal_max = panel->bl_config.bl_max_level / 2;
	bool hbm = *bl_lvl > normal_max;
	u32 dbv = hbm ? *bl_lvl - normal_max - 1 : *bl_lvl;

	mi->last_bl_lvl = *bl_lvl;

	/* Applied when local HBM is turned off */
	if (mi->lhbm_nits)
		return true;

	/* Doze brightness is part of the doze commands */
	if (mi->doze && dsi_panel_mi_has_doze(panel)) {
		if (*bl_lvl)
			dsi_panel_mi_set_doze(panel, true);
		return true;
	}

	if (mi->gamma_pending)
		dsi_panel_mi_update_gamma(panel);

	if (*bl_lvl && !mi->bl_on)
		dsi_panel_mi_queue_dimming(panel);
	mi->bl_on = *bl_lvl;

	if (mi->dbv_skip[0] && *bl_lvl >= mi->dbv_skip[0] &&
	    *bl_lvl <= mi->dbv_skip[1])
		*bl_lvl = mi->dbv_skip[0] - 1;

	if (mi->bl_sync_threshold &&
	    (*bl_lvl > mi->bl_sync_threshold) != mi->bl_sync) {
		mi->bl_sync = !mi->bl_sync;
		dsi_panel_tx_cmd_set(panel, mi->bl_sync ?
				DSI_CMD_SET_MI_BL_SYNC_ON :
				DSI_CMD_SET_MI_BL_SYNC_OFF);
	}

	if (!mi->hbm_reg53)
		return false;

	if (hbm != mi->hbm_active) {
		if (!dsi_panel_mi_tx_with_dbv(panel, hbm ? DSI_CMD_SET_MI_HBM_ON :
				DSI_CMD_SET_MI_HBM_OFF, dbv)) {
			mi->hbm_active = hbm;
			return true;
		}

		DSI_ERR("[%s] failed to switch hbm %s\n", panel->name,
				hbm ? "on" : "off");
	}

	if (mi->hbm_active)
		*bl_lvl = hbm ? dbv : 0;
	else
		*bl_lvl = min(*bl_lvl, normal_max);

	return false;
}

void dsi_panel_mi_leave_hbm(struct dsi_panel *panel)
{
	struct dsi_panel_mi *mi = &panel->mi;

	if (!mi->hbm_active)
		return;

	if (dsi_panel_mi_tx_with_dbv(panel, DSI_CMD_SET_MI_HBM_OFF,
			panel->bl_config.bl_max_level / 2))
		DSI_ERR("[%s] failed to leave hbm\n", panel->name);

	mi->hbm_active = false;
}

/* Called when the panel is turned on */
void dsi_panel_mi_reset_state(struct dsi_panel *panel)
{
	panel->mi.hbm_active = false;
	panel->mi.doze = false;
	panel->mi.doze_active = DSI_PANEL_MI_DOZE_OFF;
	panel->mi.lhbm_nits = 0;
	panel->mi.bl_sync = false;
	panel->mi.gamma_updated = false;
	panel->mi.gamma_pending = false;
	panel->mi.bl_on = false;
	if (panel->cur_mode)
		panel->mi.last_fps = panel->cur_mode->timing.refresh_rate;
}

bool dsi_panel_mi_has_doze(struct dsi_panel *panel)
{
	if (!panel->cur_mode || !panel->cur_mode->priv_info)
		return false;

	return panel->cur_mode->priv_info->cmd_sets[DSI_CMD_SET_MI_DOZE_LBM].count;
}

static int dsi_panel_mi_set_dbv_lp(struct dsi_panel *panel, u32 dbv)
{
	struct mipi_dsi_device *dsi = &panel->mipi_device;
	unsigned long mode_flags = dsi->mode_flags;
	int rc;

	dsi->mode_flags |= MIPI_DSI_MODE_LPM;
	rc = mipi_dsi_dcs_set_display_brightness(dsi, dbv);
	dsi->mode_flags = mode_flags;

	return rc < 0 ? rc : 0;
}

static enum dsi_panel_mi_doze dsi_panel_mi_doze_target(struct dsi_panel_mi *mi)
{
	return mi->last_bl_lvl > mi->doze_hbm_threshold ?
			DSI_PANEL_MI_DOZE_HBM : DSI_PANEL_MI_DOZE_LBM;
}

int dsi_panel_mi_set_doze(struct dsi_panel *panel, bool doze)
{
	struct dsi_panel_mi *mi = &panel->mi;
	enum dsi_panel_mi_doze target = DSI_PANEL_MI_DOZE_OFF;
	enum dsi_cmd_set_type type;
	int rc;

	mi->doze = doze;

	/* Applied when local HBM is turned off */
	if (mi->lhbm_nits)
		return 0;

	if (doze)
		target = dsi_panel_mi_doze_target(mi);

	if (target == mi->doze_active)
		return 0;

	switch (target) {
	case DSI_PANEL_MI_DOZE_HBM:
		type = DSI_CMD_SET_MI_DOZE_HBM;
		break;
	case DSI_PANEL_MI_DOZE_LBM:
		type = DSI_CMD_SET_MI_DOZE_LBM;
		if (mi->batch >= 0 && mi->batch < mi->batch_doze_lbm[0]) {
			u8 val = mi->batch_doze_lbm[1];

			dsi_panel_mi_update_cmd_set(panel, type, 0x53, &val, 1);
		}
		break;
	default:
		type = DSI_CMD_SET_NOLP;
		if (mi->doze_nolp)
			type = mi->doze_active == DSI_PANEL_MI_DOZE_HBM ?
					DSI_CMD_SET_MI_DOZE_HBM_NOLP :
					DSI_CMD_SET_MI_DOZE_LBM_NOLP;
		break;
	}

	if (target != DSI_PANEL_MI_DOZE_OFF && mi->doze_cmd_fps &&
	    panel->cur_mode->timing.refresh_rate != mi->doze_cmd_fps)
		rc = dsi_panel_mi_set_dbv_lp(panel, mi->doze_fallback_dbv[
				target == DSI_PANEL_MI_DOZE_HBM ? 0 : 1]);
	else
		rc = dsi_panel_tx_cmd_set(panel, type);
	if (rc)
		DSI_ERR("[%s] failed to set doze mode %d, rc=%d\n",
				panel->name, target, rc);
	else if (target == DSI_PANEL_MI_DOZE_OFF)
		dsi_panel_mi_queue_dimming(panel);

	mi->doze_active = target;

	return rc;
}

static void dsi_panel_mi_put_be(u8 *buf, u32 val, size_t len)
{
	int i;

	for (i = len - 1; i >= 0; i--, val >>= 8)
		buf[i] = val & 0xff;
}

static void dsi_panel_mi_update_param(struct dsi_panel *panel,
		enum dsi_cmd_set_type type, u8 reg, u32 val)
{
	struct dsi_panel_cmd_set *set =
			&panel->cur_mode->priv_info->cmd_sets[type];
	u8 buf[4];
	int i;

	for (i = 0; i < set->update_count; i++) {
		if (set->updates[i].reg != reg ||
		    set->updates[i].len > sizeof(buf))
			continue;

		dsi_panel_mi_put_be(buf, val, set->updates[i].len);
		dsi_panel_mi_update_cmd_set(panel, type, reg, buf,
				set->updates[i].len);
		return;
	}
}

/* DBV of the normal mode backlight range */
static u32 dsi_panel_mi_normal_dbv(struct dsi_panel *panel)
{
	struct dsi_panel_mi *mi = &panel->mi;

	if (!mi->hbm_reg53)
		return mi->last_bl_lvl;

	return min(mi->last_bl_lvl, panel->bl_config.bl_max_level / 2);
}

static int dsi_panel_mi_lhbm_on(struct dsi_panel *panel)
{
	struct dsi_panel_mi *mi = &panel->mi;
	enum dsi_cmd_set_type type;
	u32 dbv, alpha;
	int rc;

	if (mi->doze_active != DSI_PANEL_MI_DOZE_OFF) {
		type = DSI_CMD_SET_MI_LHBM_HLPM_WHITE_1000NIT;
		dbv = mi->doze_active == DSI_PANEL_MI_DOZE_HBM ?
				mi->doze_hbm_dbv : mi->doze_lbm_dbv;
	} else {
		type = DSI_CMD_SET_MI_LHBM_NORMAL_WHITE_1000NIT;
		dbv = dsi_panel_mi_normal_dbv(panel);
	}

	dsi_panel_mi_update_param(panel, type, MIPI_DCS_SET_DISPLAY_BRIGHTNESS,
			dbv);

	if (mi->lhbm_alpha) {
		alpha = dbv < mi->lhbm_alpha_size ? mi->lhbm_alpha[dbv] :
				mi->lhbm_alpha_max;
		dsi_panel_mi_update_param(panel, type, 0x87,
				alpha | mi->lhbm_alpha_or);
	}

	if (mi->lhbm_df_min)
		dsi_panel_mi_update_param(panel, type, 0xdf,
				max(dbv * 4, mi->lhbm_df_min));

	if (mi->lhbm_b2_min)
		dsi_panel_mi_update_param(panel, type, 0xb2,
				max(dbv, mi->lhbm_b2_min));

	if (mi->lhbm_white_valid)
		dsi_panel_mi_update_cmd_set(panel, type, 0xd0, mi->lhbm_white,
				sizeof(mi->lhbm_white));

	rc = dsi_panel_tx_cmd_set(panel, type);
	if (rc)
		return rc;

	/* Local HBM commands rewrite 0x53 and leave doze */
	mi->hbm_active = false;
	mi->doze_active = DSI_PANEL_MI_DOZE_OFF;

	return 0;
}

static int dsi_panel_mi_lhbm_off(struct dsi_panel *panel)
{
	struct dsi_panel_mi *mi = &panel->mi;
	u32 normal_max = panel->bl_config.bl_max_level / 2;
	enum dsi_panel_mi_doze doze = DSI_PANEL_MI_DOZE_OFF;
	int rc;

	if (mi->doze)
		doze = dsi_panel_mi_doze_target(mi);

	if (doze != DSI_PANEL_MI_DOZE_OFF) {
		rc = dsi_panel_tx_cmd_set(panel, doze == DSI_PANEL_MI_DOZE_HBM ?
				DSI_CMD_SET_MI_LHBM_OFF_TO_HLPM :
				DSI_CMD_SET_MI_LHBM_OFF_TO_LLPM);
		if (!rc)
			mi->doze_active = doze;
		return rc;
	}

	if (mi->hbm_reg53 && mi->last_bl_lvl > normal_max) {
		dsi_panel_mi_update_param(panel, DSI_CMD_SET_MI_LHBM_OFF_TO_HBM,
				MIPI_DCS_SET_DISPLAY_BRIGHTNESS,
				mi->last_bl_lvl - normal_max - 1);
		rc = dsi_panel_tx_cmd_set(panel, DSI_CMD_SET_MI_LHBM_OFF_TO_HBM);
		if (!rc)
			mi->hbm_active = true;
		return rc;
	}

	dsi_panel_mi_update_param(panel, DSI_CMD_SET_MI_LHBM_OFF_TO_NORMAL,
			MIPI_DCS_SET_DISPLAY_BRIGHTNESS,
			dsi_panel_mi_normal_dbv(panel));
	rc = dsi_panel_tx_cmd_set(panel, DSI_CMD_SET_MI_LHBM_OFF_TO_NORMAL);
	if (rc)
		return rc;

	if (!mi->hbm_reg53) {
		dsi_panel_mi_queue_dimming(panel);
		return 0;
	}

	/* Restore DDIC dimming, which local HBM turned off */
	return dsi_panel_mi_tx_with_dbv(panel, DSI_CMD_SET_MI_HBM_OFF,
			dsi_panel_mi_normal_dbv(panel));
}

/* Called with panel_lock held */
int dsi_panel_mi_set_lhbm(struct dsi_panel *panel, u32 nits)
{
	struct dsi_panel_mi *mi = &panel->mi;
	int rc;

	if (!mi->lhbm)
		return -ENODEV;

	if (!panel->panel_initialized || !panel->cur_mode ||
	    !panel->cur_mode->priv_info)
		return -EAGAIN;

	if (nits == mi->lhbm_nits)
		return 0;

	rc = nits ? dsi_panel_mi_lhbm_on(panel) :
			dsi_panel_mi_lhbm_off(panel);
	if (rc) {
		DSI_ERR("[%s] failed to set local hbm %u, rc=%d\n",
				panel->name, nits, rc);
		return rc;
	}

	mi->lhbm_nits = nits;

	return 0;
}

void dsi_panel_mi_esd_irq_ctrl(struct dsi_panel *panel, bool enable)
{
	struct dsi_panel_mi *mi = &panel->mi;

	if (!mi->esd_irq || mi->esd_irq_enabled == enable)
		return;

	if (enable) {
		enable_irq_wake(mi->esd_irq);
		enable_irq(mi->esd_irq);
	} else {
		disable_irq_wake(mi->esd_irq);
		disable_irq_nosync(mi->esd_irq);
	}

	mi->esd_irq_enabled = enable;
}

enum dsi_cmd_set_type dsi_panel_mi_timing_switch_type(struct dsi_panel *panel)
{
	struct dsi_panel_cmd_set *sets = panel->cur_mode->priv_info->cmd_sets;

	if (panel->mi.last_fps == 90 &&
	    sets[DSI_CMD_SET_MI_EXIT_90FPS_TIMING_SWITCH].count)
		return DSI_CMD_SET_MI_EXIT_90FPS_TIMING_SWITCH;

	return DSI_CMD_SET_TIMING_SWITCH;
}

static void dsi_panel_mi_update_gamma(struct dsi_panel *panel)
{
	struct dsi_panel_mi *mi = &panel->mi;
	u32 vsync_us = mi->last_fps ? USEC_PER_SEC / mi->last_fps : 0;

	/* The timing switch needs to take effect first */
	if (ktime_us_delta(ktime_get(), mi->switch_time) <= vsync_us)
		usleep_range(vsync_us, vsync_us + 10);

	if (dsi_panel_tx_cmd_set(panel, DSI_CMD_SET_MI_AUTO_UPDATE_GAMMA)) {
		DSI_ERR("[%s] failed to update gamma\n", panel->name);
		return;
	}

	mi->gamma_updated = true;
	mi->gamma_pending = false;
}

void dsi_panel_mi_post_timing_switch(struct dsi_panel *panel)
{
	struct dsi_panel_mi *mi = &panel->mi;
	struct dsi_panel_cmd_set *sets = panel->cur_mode->priv_info->cmd_sets;

	mi->switch_time = ktime_get();

	/* Update right away after the first switch, later with the backlight */
	if (!mi->doze && sets[DSI_CMD_SET_MI_AUTO_UPDATE_GAMMA].count) {
		if (!mi->gamma_updated)
			dsi_panel_mi_update_gamma(panel);
		else
			mi->gamma_pending = true;
	}

	mi->last_fps = panel->cur_mode->timing.refresh_rate;
}

void dsi_panel_mi_read_batch(struct dsi_panel *panel)
{
	struct dsi_panel_mi *mi = &panel->mi;
	struct mipi_dsi_device *dsi = &panel->mipi_device;
	unsigned long mode_flags = dsi->mode_flags;
	u8 batch;

	if (!mi->batch_doze_lbm[0] || mi->batch >= 0)
		return;

	dsi->mode_flags |= MIPI_DSI_MODE_LPM;
	if (mipi_dsi_dcs_read(dsi, 0xda, &batch, 1) == 1) {
		mi->batch = batch;
		DSI_INFO("[%s] panel batch 0x%02x\n", panel->name, batch);
	}
	dsi->mode_flags = mode_flags;
}

void dsi_panel_mi_deinit(struct dsi_panel *panel)
{
	cancel_delayed_work_sync(&panel->mi.dimming_work);
}

/* Called before sending the panel on commands */
void dsi_panel_mi_update_on_cmds(struct dsi_panel *panel)
{
	struct dsi_panel_mi *mi = &panel->mi;
	struct dsi_panel_cmd_set *set;
	u32 reg, index, len, offset = 0;
	u8 *payload;
	int i;

	if (!mi->vdc_update_count || !panel->cur_mode ||
	    !panel->cur_mode->priv_info)
		return;

	set = &panel->cur_mode->priv_info->cmd_sets[DSI_CMD_SET_ON];
	for (i = 0; i < mi->vdc_update_count; i += 3) {
		reg = mi->vdc_update[i];
		index = mi->vdc_update[i + 1];
		len = mi->vdc_update[i + 2];
		if (index >= set->count || offset + len > sizeof(mi->vdc_param))
			break;

		payload = (u8 *)set->cmds[index].msg.tx_buf;
		if (set->cmds[index].msg.tx_len < len + 1 || payload[0] != reg) {
			DSI_ERR("[%s] invalid vdc update of 0x%02x\n",
					panel->name, reg);
			break;
		}

		memcpy(&payload[1], &mi->vdc_param[offset], len);
		offset += len;
	}
}

static enum dsi_cmd_set_type dsi_panel_mi_fps_gamma_type(struct dsi_panel *panel)
{
	switch (panel->cur_mode->timing.refresh_rate) {
	case 60:
		return DSI_CMD_SET_MI_FPS_60_GAMMA;
	case 90:
		return DSI_CMD_SET_MI_FPS_90_GAMMA;
	case 120:
		return DSI_CMD_SET_MI_FPS_120_GAMMA;
	default:
		return DSI_CMD_SET_MAX;
	}
}

bool dsi_panel_mi_has_fps_gamma(struct dsi_panel *panel)
{
	enum dsi_cmd_set_type type;

	if (!panel->cur_mode || !panel->cur_mode->priv_info)
		return false;

	type = dsi_panel_mi_fps_gamma_type(panel);

	return type != DSI_CMD_SET_MAX &&
			panel->cur_mode->priv_info->cmd_sets[type].count;
}

/* Called with panel_lock held, after switching the video mode refresh rate */
void dsi_panel_mi_fps_gamma(struct dsi_panel *panel)
{
	if (!panel->panel_initialized || !dsi_panel_mi_has_fps_gamma(panel))
		return;

	if (dsi_panel_tx_cmd_set(panel, dsi_panel_mi_fps_gamma_type(panel)))
		DSI_ERR("[%s] failed to switch fps gamma\n", panel->name);
}

void dsi_panel_mi_override_dfps_vfp(struct dsi_panel *panel,
		struct dsi_display_mode *mode)
{
	struct dsi_panel_mi *mi = &panel->mi;
	int i;

	for (i = 0; i < mi->dfps_vfp_count; i += 2)
		if (mode->timing.refresh_rate == mi->dfps_vfp[i])
			mode->timing.v_front_porch = mi->dfps_vfp[i + 1];
}
