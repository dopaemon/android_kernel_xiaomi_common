load("//build/bazel_common_rules/dist:dist.bzl", "copy_to_dist_dir")
load("//build/kernel/kleaf:kernel.bzl", "ddk_module")

def define_wlan(target, variant):
    kernel_build_variant = "{}_{}".format(target, variant)
    include_base = "../../../{}".format(native.package_name())

    ddk_module(
        name = "{}_wlan".format(kernel_build_variant),
        out = "rmnet_wlan.ko",
        srcs = [
            "rmnet_wlan.h",
            "rmnet_wlan_connection.c",
            "rmnet_wlan_connection.h",
            "rmnet_wlan_fragment.c",
            "rmnet_wlan_fragment.h",
            "rmnet_wlan_genl.c",
            "rmnet_wlan_genl.h",
            "rmnet_wlan_main.c",
            "rmnet_wlan_stats.c",
            "rmnet_wlan_stats.h",
        ],
        kernel_build = "//vendor/xiaomi/sm8450-kernel:{}".format(kernel_build_variant),
        deps = [
            "//vendor/xiaomi/sm8450-kernel:all_headers",
            "//vendor/xiaomi/sm8450-modules/qcom/opensource/datarmnet:{}_rmnet_core".format(kernel_build_variant),
            "//vendor/xiaomi/sm8450-modules/qcom/opensource/datarmnet:rmnet_core_headers",
        ],
        copts = ["-Wno-misleading-indentation"],
    )

    copy_to_dist_dir(
        name = "{}_datarment-ext_dist".format(kernel_build_variant),
        data = [
            ":{}_wlan".format(kernel_build_variant),
        ],
        dist_dir = "out/target/product/{}/dlkm/lib/modules/".format(target),
        flat = True,
        wipe_dist_dir = False,
        allow_duplicate_filenames = False,
        mode_overrides = {"**/*": "644"},
    )
