TARGET_SYNX_ENABLE := false
CONFIG_MSM_GLOBAL_SYNX_V2 := false

ifeq ($(TARGET_KERNEL_DLKM_DISABLE),true)
	ifeq ($(TARGET_KERNEL_DLKM_SYNX_OVERRIDE),true)
		TARGET_SYNX_ENABLE := true
	endif
else
TARGET_SYNX_ENABLE := true
endif

ifeq ($(TARGET_SYNX_ENABLE),true)
SYNX_BLD_DIR := $(TOP)/vendor/qcom/opensource/synx-kernel

ifeq ($(TARGET_BOARD_PLATFORM), anorak61)
CONFIG_MSM_GLOBAL_SYNX_V2 := true
endif

# Build synx-driver.ko
###########################################################
# This is set once per LOCAL_PATH, not per (kernel) module
KBUILD_OPTIONS := SYNX_ROOT=$(SYNX_BLD_DIR)
KBUILD_OPTIONS += BOARD_PLATFORM=$(TARGET_BOARD_PLATFORM)
###########################################################

DLKM_DIR   := $(TOP)/device/qcom/common/dlkm

LOCAL_PATH := $(call my-dir)
LOCAL_MODULE_DDK_BUILD := true
LOCAL_MODULE_KO_DIRS := msm/synx-driver.ko
ifeq ($(CONFIG_MSM_GLOBAL_SYNX_V2),true)
LOCAL_MODULE_KO_DIRS += msm/synx/ipclite.ko
else
LOCAL_MODULE_KO_DIRS += msm/synx/ipclite.ko
LOCAL_MODULE_KO_DIRS += msm/synx/qcom_ipc_lite.ko
endif

include $(CLEAR_VARS)
# For incremental compilation
LOCAL_SRC_FILES           := $(wildcard $(LOCAL_PATH)/**/*) $(wildcard $(LOCAL_PATH)/*)
LOCAL_MODULE              := synx-driver-symvers
LOCAL_MODULE_KBUILD_NAME  := Module.symvers
#LOCAL_MODULE_STEM         := Module.symvers
LOCAL_MODULE_PATH         := $(KERNEL_MODULES_OUT)
include $(DLKM_DIR)/Build_external_kernelmodule.mk

include $(CLEAR_VARS)
# For incremental compilation
LOCAL_SRC_FILES   := $(wildcard $(LOCAL_PATH)/**/*) $(wildcard $(LOCAL_PATH)/*)
$(info LOCAL_SRC_FILES = $(LOCAL_SRC_FILES))
LOCAL_MODULE      := synx-driver.ko
LOCAL_MODULE_KBUILD_NAME := msm/synx-driver.ko
LOCAL_MODULE_PATH := $(KERNEL_MODULES_OUT)
BOARD_VENDOR_KERNEL_MODULES += $(LOCAL_MODULE_PATH)/$(LOCAL_MODULE)
include $(DLKM_DIR)/Build_external_kernelmodule.mk

include $(CLEAR_VARS)
# For incremental compilation
ifeq ($(CONFIG_MSM_GLOBAL_SYNX_V2),true)
LOCAL_SRC_FILES   := $(wildcard $(LOCAL_PATH)/**/*) $(wildcard $(LOCAL_PATH)/*)
$(info LOCAL_SRC_FILES = $(LOCAL_SRC_FILES))
LOCAL_MODULE      := ipclite.ko
LOCAL_MODULE_KBUILD_NAME := msm/synx/ipclite.ko
LOCAL_MODULE_PATH := $(KERNEL_MODULES_OUT)
BOARD_VENDOR_KERNEL_MODULES += $(LOCAL_MODULE_PATH)/$(LOCAL_MODULE)
include $(DLKM_DIR)/Build_external_kernelmodule.mk
else
LOCAL_SRC_FILES   := $(wildcard $(LOCAL_PATH)/**/*) $(wildcard $(LOCAL_PATH)/*)
$(info LOCAL_SRC_FILES = $(LOCAL_SRC_FILES))
LOCAL_MODULE      := qcom_ipc_lite.ko
LOCAL_MODULE_KBUILD_NAME := msm/synx/qcom_ipc_lite.ko
LOCAL_MODULE_PATH := $(KERNEL_MODULES_OUT)
BOARD_VENDOR_KERNEL_MODULES += $(LOCAL_MODULE_PATH)/$(LOCAL_MODULE)
include $(DLKM_DIR)/Build_external_kernelmodule.mk

include $(CLEAR_VARS)
LOCAL_SRC_FILES   := $(wildcard $(LOCAL_PATH)/**/*) $(wildcard $(LOCAL_PATH)/*)
$(info LOCAL_SRC_FILES = $(LOCAL_SRC_FILES))
LOCAL_MODULE      := ipclite.ko
LOCAL_MODULE_KBUILD_NAME := msm/synx/ipclite.ko
LOCAL_MODULE_PATH := $(KERNEL_MODULES_OUT)
BOARD_VENDOR_KERNEL_MODULES += $(LOCAL_MODULE_PATH)/$(LOCAL_MODULE)
include $(DLKM_DIR)/Build_external_kernelmodule.mk
endif

# print out variables
$(info KBUILD_OPTIONS = $(KBUILD_OPTIONS))
$(info LOCAL_ADDITIONAL_DEPENDENCY = $(LOCAL_ADDITIONAL_DEPENDENCY))
$(info LOCAL_ADDITIONAL_DEPENDENCIES = $(LOCAL_ADDITIONAL_DEPENDENCIES))
$(info LOCAL_REQUIRED_MODULES = $(LOCAL_REQUIRED_MODULES))
$(info DLKM_DIR = $(DLKM_DIR))
endif # End of check for TARGET_SYNX_ENABLE
