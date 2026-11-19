load(":synx_modules.bzl", "synx_modules")
load(":synx_module_build.bzl", "define_consolidate_perf_modules")

def define_anorak61():
    define_consolidate_perf_modules(
        target = "anorak",
        registry = synx_modules,
        modules = [
            "synx-driver",
            "ipclite",
        ],
        config_options = [
            "TARGET_SYNX_ENABLE",
	    "CONFIG_MSM_GLOBAL_SYNX_V2"
        ],
    )
