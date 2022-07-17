KERNEL_SRC ?= /lib/modules/$(shell uname -r)/build

# The Make variable $(M) must point to the directory that contains the module
# source code (which includes this Makefile). It can either be an absolute or a
# relative path. If it is a relative path, then it must be relative to the
# kernel source directory (KERNEL_SRC). An absolute path can be obtained very
# easily through $(shell pwd). Generating a path relative to KERNEL_SRC is
# difficult and we accept some outside help by letting the caller override the
# variable $(M). Allowing a relative path for $(M) enables us to have the build
# system put output/object files (.o, .ko.) into a directory different from the
# module source directory.
M ?= $(shell pwd)

ifeq ($(WLAN_ROOT),)
WLAN_CHIPSET := qca_cld3
LOCAL_DEV_NAME := qca6490

# WLAN_ROOT must contain an absolute path (i.e. not a relative path)
KBUILD_OPTIONS := WLAN_ROOT=$(KERNEL_SRC)/$(M)

# MODNAME should be qca_cld3_wlan for helium based platform
ifeq (qca_cld3, $(WLAN_CHIPSET))
KBUILD_OPTIONS += MODNAME?=$(WLAN_CHIPSET)_$(LOCAL_DEV_NAME)
else
KBUILD_OPTIONS += MODNAME?=wlan
endif

#By default build for CLD
WLAN_SELECT := CONFIG_QCA_CLD_WLAN=m
KBUILD_OPTIONS += $(WLAN_SELECT)

KBUILD_OPTIONS += \
    WLAN_PROFILE=$(LOCAL_DEV_NAME) \
    DYNAMIC_SINGLE_CHIP=$(LOCAL_DEV_NAME) \
    MODNAME=$(WLAN_CHIPSET)_$(LOCAL_DEV_NAME) \
    DEVNAME=$(LOCAL_DEV_NAME) \
    WLAN_CTRL_NAME=wlan

endif

all:
	$(MAKE) -C $(KERNEL_SRC) M=$(M) modules $(KBUILD_OPTIONS)

modules_install:
	$(MAKE) INSTALL_MOD_STRIP=1 M=$(M) -C $(KERNEL_SRC) modules_install

clean:
	$(MAKE) -C $(KERNEL_SRC) M=$(M) clean $(KBUILD_OPTIONS)
