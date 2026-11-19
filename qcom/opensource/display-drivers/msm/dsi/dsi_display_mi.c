// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (C) 2026 The LineageOS Project
 */

#include <linux/delay.h>
#include <linux/sched.h>
#include <linux/sysfs.h>

#include "dsi_display.h"
#include "dsi_display_mi.h"

static void dsi_display_mi_set_fod_ui(struct dsi_display *display, bool fod_ui)
{
	struct dsi_display_mi *mi = &display->mi;
	bool changed;

	mutex_lock(&mi->lock);
	changed = mi->fod_ui != fod_ui;
	mi->fod_ui = fod_ui;
	mutex_unlock(&mi->lock);

	if (changed)
		sysfs_notify(&display->pdev->dev.kobj, NULL, "fod_ui");
}

static void dsi_display_mi_lhbm_work(struct kthread_work *work)
{
	struct dsi_display_mi *mi =
			container_of(work, struct dsi_display_mi, lhbm_work);
	struct dsi_display *display =
			container_of(mi, struct dsi_display, mi);
	struct dsi_panel *panel = display->panel;
	u32 target, frames, fps = 0;
	bool allowed;
	int rc;

	mutex_lock(&mi->lock);
	target = mi->lhbm_target;
	allowed = mi->allowed;
	mutex_unlock(&mi->lock);

	if (!allowed)
		return;

	if (!target)
		dsi_display_mi_set_fod_ui(display, false);

	mutex_lock(&panel->panel_lock);
	rc = dsi_panel_mi_set_lhbm(panel, target);
	if (panel->cur_mode)
		fps = panel->cur_mode->timing.refresh_rate;
	mutex_unlock(&panel->panel_lock);

	if (rc || !target)
		return;

	frames = panel->mi.lhbm_ui_ready_frames;
	if (frames && fps)
		usleep_range(USEC_PER_SEC / fps * frames,
				USEC_PER_SEC / fps * frames + 10);

	mutex_lock(&mi->lock);
	target = mi->lhbm_target == target;
	mutex_unlock(&mi->lock);

	if (target)
		dsi_display_mi_set_fod_ui(display, true);
}

void dsi_display_mi_set_allowed(struct dsi_display *display, bool allowed)
{
	struct dsi_display_mi *mi = &display->mi;

	if (!mi->worker)
		return;

	mutex_lock(&mi->lock);
	mi->allowed = allowed;
	mutex_unlock(&mi->lock);

	if (allowed)
		kthread_queue_work(mi->worker, &mi->lhbm_work);
	else
		dsi_display_mi_set_fod_ui(display, false);
}

static const char * const dsi_display_mi_state_names[] = {
	[DSI_DISPLAY_MI_POWER] = "panel_power_state",
	[DSI_DISPLAY_MI_FPS] = "dynamic_fps",
	[DSI_DISPLAY_MI_DBV] = "dbv",
};

void dsi_display_mi_notify(struct dsi_display *display,
		enum dsi_display_mi_state state, u32 value)
{
	struct dsi_display_mi *mi = &display->mi;
	bool changed;

	mutex_lock(&mi->lock);
	changed = mi->state[state] != value;
	mi->state[state] = value;
	mutex_unlock(&mi->lock);

	if (changed)
		sysfs_notify(&display->pdev->dev.kobj, NULL,
				dsi_display_mi_state_names[state]);
}

static ssize_t dsi_display_mi_state_show(struct device *dev, char *buf,
		enum dsi_display_mi_state state)
{
	struct dsi_display *display = dev_get_drvdata(dev);
	u32 value;

	mutex_lock(&display->mi.lock);
	value = display->mi.state[state];
	mutex_unlock(&display->mi.lock);

	return sysfs_emit(buf, "%u\n", value);
}

static ssize_t panel_power_state_show(struct device *dev,
		struct device_attribute *attr, char *buf)
{
	return dsi_display_mi_state_show(dev, buf, DSI_DISPLAY_MI_POWER);
}

static ssize_t dynamic_fps_show(struct device *dev,
		struct device_attribute *attr, char *buf)
{
	return dsi_display_mi_state_show(dev, buf, DSI_DISPLAY_MI_FPS);
}

static ssize_t dbv_show(struct device *dev,
		struct device_attribute *attr, char *buf)
{
	return dsi_display_mi_state_show(dev, buf, DSI_DISPLAY_MI_DBV);
}

static DEVICE_ATTR_RO(panel_power_state);
static DEVICE_ATTR_RO(dynamic_fps);
static DEVICE_ATTR_RO(dbv);

static struct attribute *dsi_display_mi_state_attrs[] = {
	&dev_attr_panel_power_state.attr,
	&dev_attr_dynamic_fps.attr,
	&dev_attr_dbv.attr,
	NULL,
};

static const struct attribute_group dsi_display_mi_state_group = {
	.attrs = dsi_display_mi_state_attrs,
};

static ssize_t local_hbm_show(struct device *dev,
		struct device_attribute *attr, char *buf)
{
	struct dsi_display *display = dev_get_drvdata(dev);
	u32 target;

	mutex_lock(&display->mi.lock);
	target = display->mi.lhbm_target;
	mutex_unlock(&display->mi.lock);

	return sysfs_emit(buf, "%u\n", target);
}

static ssize_t local_hbm_store(struct device *dev,
		struct device_attribute *attr, const char *buf, size_t count)
{
	struct dsi_display *display = dev_get_drvdata(dev);
	struct dsi_display_mi *mi = &display->mi;
	u32 nits;
	int rc;

	rc = kstrtou32(buf, 0, &nits);
	if (rc)
		return rc;

	if (nits && nits != 1000)
		return -EINVAL;

	mutex_lock(&mi->lock);
	mi->lhbm_target = nits;
	mutex_unlock(&mi->lock);

	kthread_queue_work(mi->worker, &mi->lhbm_work);

	return count;
}

static ssize_t fod_ui_show(struct device *dev,
		struct device_attribute *attr, char *buf)
{
	struct dsi_display *display = dev_get_drvdata(dev);
	bool fod_ui;

	mutex_lock(&display->mi.lock);
	fod_ui = display->mi.fod_ui;
	mutex_unlock(&display->mi.lock);

	return sysfs_emit(buf, "%d\n", fod_ui);
}

static DEVICE_ATTR_RW(local_hbm);
static DEVICE_ATTR_RO(fod_ui);

static struct attribute *dsi_display_mi_lhbm_attrs[] = {
	&dev_attr_local_hbm.attr,
	&dev_attr_fod_ui.attr,
	NULL,
};

static const struct attribute_group dsi_display_mi_lhbm_group = {
	.attrs = dsi_display_mi_lhbm_attrs,
};

void dsi_display_mi_init(struct dsi_display *display)
{
	struct dsi_display_mi *mi = &display->mi;

	mutex_init(&mi->lock);

	mi->state[DSI_DISPLAY_MI_POWER] = SDE_MODE_DPMS_OFF;
	if (sysfs_create_group(&display->pdev->dev.kobj,
			&dsi_display_mi_state_group))
		DSI_ERR("failed to create state sysfs nodes\n");

	if (!display->panel->mi.lhbm)
		return;

	mi->worker = kthread_create_worker(0, "dsi_lhbm_%s",
			display->display_type);
	if (IS_ERR(mi->worker)) {
		DSI_ERR("failed to create local hbm worker\n");
		mi->worker = NULL;
		return;
	}
	sched_set_fifo(mi->worker->task);
	kthread_init_work(&mi->lhbm_work, dsi_display_mi_lhbm_work);

	if (sysfs_create_group(&display->pdev->dev.kobj,
			&dsi_display_mi_lhbm_group))
		DSI_ERR("failed to create local hbm sysfs nodes\n");
}

void dsi_display_mi_deinit(struct dsi_display *display)
{
	struct dsi_display_mi *mi = &display->mi;

	sysfs_remove_group(&display->pdev->dev.kobj,
			&dsi_display_mi_state_group);

	if (!mi->worker)
		return;

	sysfs_remove_group(&display->pdev->dev.kobj,
			&dsi_display_mi_lhbm_group);
	kthread_destroy_worker(mi->worker);
	mi->worker = NULL;
}
