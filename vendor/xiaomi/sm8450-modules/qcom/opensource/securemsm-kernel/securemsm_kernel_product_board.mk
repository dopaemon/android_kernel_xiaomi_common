#Build ssg kernel driver

ENABLE_SECUREMSM_DLKM := true
ENABLE_SECUREMSM_QTEE_DLKM := true
ENABLE_QCEDEV_FE := false
ENABLE_SST_INVOKE_TEST := false
ENABLE_HDCP_TEST := false
ENABLE_SECCAM_TEST := false
ENABLE_SI_CORE_TEST := false

ifeq ($(TARGET_KERNEL_DLKM_DISABLE), true)
  ifeq ($(TARGET_KERNEL_DLKM_SECURE_MSM_OVERRIDE), false)
      ENABLE_SECUREMSM_DLKM := false
  endif
  ifeq ($(TARGET_KERNEL_DLKM_SECUREMSM_QTEE_OVERRIDE), false)
      ENABLE_SECUREMSM_QTEE_DLKM := false
  endif
endif

#enable QCEDEV_FE driver only on Automotive Lemans HQX/HGY LA GVM.
ifeq ($(ENABLE_HYP),true)
  ifneq ($(filter $(TARGET_BOARD_PLATFORM),gen4 gen5),)
      ENABLE_QCEDEV_FE := true
  endif #TARGET_BOARD_PLATFORM
endif #ENABLE_HYP

ifeq ($(ENABLE_SECUREMSM_DLKM), true)
  ENABLE_QCRYPTO_DLKM := true
  ENABLE_HDCP_QSEECOM_DLKM := true
  ENABLE_QRNG_DLKM := true
  ifeq ($(TARGET_USES_SMMU_PROXY), true)
    ENABLE_SMMU_PROXY := true
  endif #TARGET_USES_SMMU_PROXY
endif #ENABLE_SECUREMSM_DLKM

ifeq ($(ENABLE_SECUREMSM_QTEE_DLKM), true)
  ENABLE_SMCINVOKE_DLKM := true
  ENABLE_TZLOG_DLKM := true
  #Enable Qseecom if TARGET_ENABLE_QSEECOM or TARGET_BOARD_AUTO is set to true
  ifneq (, $(filter true, $(TARGET_ENABLE_QSEECOM) $(TARGET_BOARD_AUTO)))
    ENABLE_QSEECOM_DLKM := true
  endif #TARGET_ENABLE_QSEECOM OR TARGET_BOARD_AUTO
endif #ENABLE_SECUREMSM_QTEE_DLKM

ifeq ($(TARGET_USES_GY), true)
  ENABLE_QCRYPTO_DLKM := false
  ENABLE_HDCP_QSEECOM_DLKM := false
  ENABLE_QRNG_DLKM := true
  ENABLE_SMMU_PROXY := false
  ENABLE_SMCINVOKE_DLKM := true
  ENABLE_TZLOG_DLKM := false
  ENABLE_QSEECOM_DLKM := false
endif #TARGET_USES_GY

# TEST Drivers (si_core_test, seccam_driver, hdcp_test, tornado_mod)
ifneq ($(TARGET_USES_QMAA), true)
    ENABLE_SST_INVOKE_TEST := true
    ENABLE_HDCP_TEST := true
    ENABLE_SI_CORE_TEST := true
    ENABLE_SECCAM_TEST := true
endif

ifeq ($(TARGET_USES_QMAA_OVERRIDE_SST_CLIENTS), true)
    ENABLE_SST_INVOKE_TEST := true
endif
ifeq ($(TARGET_KERNEL_DLKM_SECURE_MSM_OVERRIDE), true)
    ENABLE_HDCP_TEST := true
    ENABLE_SECCAM_TEST := true
    ENABLE_SI_CORE_TEST := true
endif
ifeq ($(ENABLE_HDCP_DP), true)
    ENABLE_HDCP_TEST := true
endif

# Disabling test drivers for GY targets
ifeq ($(TARGET_BOARD_AUTO), true)
  ENABLE_HDCP_TEST := false
  ENABLE_SECCAM_TEST := false
  ENABLE_SI_CORE_TEST := false
  ENABLE_SST_INVOKE_TEST := false
endif #TARGET_BOARD_AUTO

# Disabling test drivers for LW targets
ifeq ($(TARGET_SUPPORTS_WEAR_OS), true)
  ENABLE_HDCP_TEST := false
  ENABLE_SECCAM_TEST := false
  ENABLE_SI_CORE_TEST := false
  ENABLE_SST_INVOKE_TEST := false
endif #TARGET_SUPPORTS_WEAR_OS

ifeq ($(ENABLE_QCRYPTO_DLKM), true)
PRODUCT_PACKAGES += qcedev-mod_dlkm.ko
PRODUCT_PACKAGES += qce50_dlkm.ko
PRODUCT_PACKAGES += qcrypto-msm_dlkm.ko
endif #ENABLE_QCRYPTO_DLKM

ifeq ($(ENABLE_HDCP_QSEECOM_DLKM), true)
PRODUCT_PACKAGES += hdcp_qseecom_dlkm.ko
endif #ENABLE_HDCP_QSEECOM_DLKM

ifeq ($(ENABLE_QRNG_DLKM), true)
PRODUCT_PACKAGES += qrng_dlkm.ko
endif #ENABLE_QRNG_DLKM

ifeq ($(ENABLE_SMMU_PROXY), true)
PRODUCT_PACKAGES += smmu_proxy_dlkm.ko
endif #ENABLE_SMMU_PROXY

ifeq ($(ENABLE_SMCINVOKE_DLKM), true)
PRODUCT_PACKAGES += smcinvoke_dlkm.ko
endif #ENABLE_SMCINVOKE_DLKM

ifeq ($(ENABLE_TZLOG_DLKM), true)
PRODUCT_PACKAGES += tz_log_dlkm.ko
endif #ENABLE_TZLOG_DLKM

ifeq ($(ENABLE_QSEECOM_DLKM), true)
PRODUCT_PACKAGES += qseecom_dlkm.ko
endif #ENABLE_QSEECOM_DLKM

ifeq ($(ENABLE_QCEDEV_FE), true)
PRODUCT_PACKAGES += qcedev_fe_dlkm.ko
endif #ENABLE_QCEDEV_FE

ifeq ($(ENABLE_SECCAM_TEST), true)
PRODUCT_PACKAGES_DEBUG += seccam_test_driver.ko
endif #ENABLE_SECCAM_TEST

ifeq ($(ENABLE_HDCP_TEST), true)
PRODUCT_PACKAGES_DEBUG += hdcp2p2_test.ko
endif #ENABLE_HDCP_TEST

ifeq ($(ENABLE_SI_CORE_TEST), true)
PRODUCT_PACKAGES_DEBUG += si_core_test.ko
endif #ENABLE_SI_CORE_TEST

ifeq ($(ENABLE_SST_INVOKE_TEST), true)
PRODUCT_PACKAGES_DEBUG += tornado_mod.ko
endif #ENABLE_SST_INVOKE_TEST
