# SPDX-License-Identifier: GPL-2.0-only
ifeq ($(CONFIG_MSM_GLOBAL_SYNX_V2), true)
obj-m += synx-driver.o
obj-m  += ipclite.o
synx-driver-y := synx_v2.o synx_util_v2.o synx_debugfs_v2.o
else
obj-m  += synx-driver.o
obj-m  += ipclite.o
obj-m  += qcom_ipc_lite.o
synx-driver-y := synx.o synx_util.o synx_debugfs.o
endif

