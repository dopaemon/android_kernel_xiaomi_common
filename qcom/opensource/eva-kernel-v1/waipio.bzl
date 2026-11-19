load(":eva_modules.bzl", "eva_modules")
load(":eva_module_build.bzl", "define_target_variant_modules")

def define_waipio():
    define_target_variant_modules(
        target = "waipio",
        variant = "perf",
        registry = eva_modules,
        modules = [
            "msm-eva",
        ],
        config_options = [
            "CONFIG_EVA_WAIPIO",
            "CONFIG_MSM_MMRM",
            "CONFIG_MSM_GLOBAL_SYNX",
            "TARGET_SYNX_ENABLE",
            "TARGET_MMRM_ENABLE",
            "TARGET_DSP_ENABLE",
        ],
    )
