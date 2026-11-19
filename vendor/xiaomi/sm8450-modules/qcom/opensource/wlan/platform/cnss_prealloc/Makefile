# SPDX-License-Identifier: GPL-2.0-only

ifeq ($(CONFIG_ARCH_PARROT), y)
include $(WLAN_PLATFORM_ROOT)/cnss_prealloc/parrot_gki_defconfig
endif

ccflags-y += -Wmissing-prototypes

ifeq ($(CONFIG_CNSS_OUT_OF_TREE),y)
ccflags-y += -I$(WLAN_PLATFORM_ROOT)/inc
ccflags-y += -I$(WLAN_PLATFORM_ROOT)/cnss_utils
endif

ifeq ($(CONFIG_CNSS_PREALLOC_DEBUG_LEAK),y)
ccflags-y += -DCONFIG_CNSS_PREALLOC_DEBUG_LEAK
endif

obj-$(CONFIG_WCNSS_MEM_PRE_ALLOC) += cnss_prealloc.o
