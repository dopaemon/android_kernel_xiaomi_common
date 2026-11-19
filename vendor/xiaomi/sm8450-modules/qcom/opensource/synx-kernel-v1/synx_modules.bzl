load(":synx_module_build.bzl", "create_module_registry")

SYNX_KERNEL_ROOT = "synx-kernel"

synx_modules = create_module_registry([":synx_headers"])
register_synx_module = synx_modules.register

register_synx_module(
    name = "synx-driver",
    path = "msm",
    config_srcs  = {
	"CONFIG_MSM_GLOBAL_SYNX_V2":[
		"synx/synx_v2.c",
		"synx/synx_util_v2.c",
		"synx/synx_global_v2.c",
		"synx/synx_debugfs_v2.c",
	],
	"CONFIG_MSM_GLOBAL_SYNX":[
		"synx/synx.c",
		"synx/synx_util.c",
		"synx/synx_debugfs.c",
	],
    }
)
register_synx_module(
    name = "ipclite",
    path = "msm",
    srcs = [
        "synx/ipclite.c",
    ],
)

register_synx_module(
    name = "qcom_ipc_lite",
    path = "msm",
    srcs = [
        "synx/qcom_ipc_lite.c",
    ],
)
