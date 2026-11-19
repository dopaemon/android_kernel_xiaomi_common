load("//build/bazel_common_rules/dist:dist.bzl", "copy_to_dist_dir")
load("//build/kernel/kleaf:kernel.bzl", "ddk_module")

def define_offload(target, variant):
    kernel_build_variant = "{}_{}".format(target, variant)
    include_base = "../../../{}".format(native.package_name())

    ddk_module(
        name = "{}_offload".format(kernel_build_variant),
        out = "rmnet_offload.ko",
        srcs = [
            "rmnet_offload_engine.c",
            "rmnet_offload_engine.h",
            "rmnet_offload_knob.c",
            "rmnet_offload_knob.h",
            "rmnet_offload_main.c",
            "rmnet_offload_main.h",
            "rmnet_offload_state.c",
            "rmnet_offload_state.h",
            "rmnet_offload_stats.c",
            "rmnet_offload_stats.h",
            "rmnet_offload_tcp.c",
            "rmnet_offload_tcp.h",
            "rmnet_offload_udp.c",
            "rmnet_offload_udp.h",
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
            ":{}_offload".format(kernel_build_variant),
        ],
        dist_dir = "out/target/product/{}/dlkm/lib/modules/".format(target),
        flat = True,
        wipe_dist_dir = False,
        allow_duplicate_filenames = False,
        mode_overrides = {"**/*": "644"},
    )
