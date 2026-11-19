ifeq ($(strip $(NFC_DLKM_ENABLED)),true)
LOCAL_PATH := $(call my-dir)
include $(CLEAR_VARS)


ifeq ($(TARGET_ENABLE_PERIPHERAL_CONTROL), true)
  LOCAL_CFLAGS := -DNFC_SECURE_PERIPHERAL_ENABLED
  KBUILD_OPTIONS += KBUILD_EXTRA_SYMBOLS=$(PWD)/$(call intermediates-dir-for,DLKM,sec-module-symvers)/Module.symvers
  ifeq ($(TARGET_KERNEL_DLKM_SECURE_MSM_OVERRIDE), true)
    LOCAL_REQUIRED_MODULES := sec-module-symvers
    LOCAL_ADDITIONAL_DEPENDENCIES += $(call intermediates-dir-for,DLKM,sec-module-symvers)/Module.symvers
  endif
endif

LOCAL_MODULE_PATH := $(KERNEL_MODULES_OUT)
LOCAL_MODULE := stm_nfc_i2c.ko
LOCAL_SRC_FILES   := $(wildcard $(LOCAL_PATH)/**/*) $(wildcard $(LOCAL_PATH)/*)

DLKM_DIR := $(TOP)/device/qcom/common/dlkm

LOCAL_MODULE_DDK_BUILD := true
include $(DLKM_DIR)/Build_external_kernelmodule.mk
endif
