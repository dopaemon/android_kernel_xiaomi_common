load(":display_modules.bzl", "display_driver_modules")
load(":display_driver_build.bzl", "define_target_variant_modules")
load("//vendor/xiaomi/sm8450-kernel:target_variants.bzl", "get_all_la_variants")

def define_waipio():
    for (t, v) in get_all_la_variants():
        if t == "waipio":
           define_target_variant_modules(
            target = t,
            variant = v,
            registry = display_driver_modules,
            modules = [
                "msm_drm",
        ],
        config_options = [
            "CONFIG_DEBUG_FS",
            "CONFIG_DRM_MSM_DP",
            "CONFIG_DRM_MSM_DP_MST",
            "CONFIG_DRM_MSM_DSI",
            "CONFIG_DRM_MSM_REGISTER_LOGGING",
            "CONFIG_DRM_MSM_SDE",
            "CONFIG_DRM_SDE_RSC",
            "CONFIG_DRM_SDE_VM",
            "CONFIG_DRM_SDE_WB",
            "CONFIG_DSI_PARSER",
            "CONFIG_DYNAMIC_DEBUG",
            "CONFIG_HDCP_QSEECOM",
            "CONFIG_MSM_EXT_DISPLAY",
            "CONFIG_MSM_MMRM",
            "CONFIG_QCOM_FSA4480_I2C",
            "CONFIG_QCOM_MDSS_PLL",
            "CONFIG_QCOM_SPEC_SYNC",
            "CONFIG_SYNC_FILE",
            "CONFIG_THERMAL_OF",
        ],
)
