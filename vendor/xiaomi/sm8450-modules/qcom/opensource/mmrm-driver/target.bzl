load(":mmrm_modules.bzl", "mmrm_driver_modules")
load(":mmrm_modules_build.bzl", "define_consolidate_gki_modules", "define_target_variant_modules")
load("//vendor/xiaomi/sm8450-kernel:target_variants.bzl", "get_all_la_variants")

def define_waipio():
    for (t, v) in get_all_la_variants():
        if t == "waipio":
            define_target_variant_modules(
                target = t,
                variant = v,
                registry = mmrm_driver_modules,
                modules = [
                    "msm-mmrm",
                ],
            )

def define_blair():
    define_consolidate_gki_modules(
        target = "blair",
        registry = mmrm_driver_modules,
        modules = [
            "msm-mmrm",
            "mmrm_test_module",
        ],
)