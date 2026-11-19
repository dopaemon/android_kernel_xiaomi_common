ifeq ($(CONFIG_ARCH_WAIPIO), y)
dtbo-y += waipio-vidc.dtbo
endif

ifeq ($(CONFIG_ARCH_CAPE), y)
dtbo-y += ukee-vidc.dtbo
endif

ifeq ($(CONFIG_ARCH_DIWALI), y)
dtbo-y += diwali-vidc.dtbo
endif

ifeq ($(CONFIG_ARCH_PARROT), y)
dtbo-y += parrot-vidc.dtbo
endif

ifeq ($(CONFIG_ARCH_NEO), y)
dtbo-y += neo-vidc.dtbo
endif

ifeq ($(CONFIG_ARCH_KHAJE), y)
dtbo-y += khaje-vidc.dtbo
endif

ifeq ($(CONFIG_ARCH_RAVELIN), y)
dtbo-y += ravelin-vidc.dtbo
endif

ifeq ($(CONFIG_ARCH_MONACO), y)
dtbo-y += monaco-vidc.dtbo
endif

ifeq ($(CONFIG_ARCH_BLAIR), y)
dtbo-y += blair-vidc.dtbo
endif

ifeq ($(CONFIG_ARCH_PITTI), y)
dtbo-y += pitti-vidc.dtbo
dtbo-y += pitti-vidc-32go.dtbo
endif

ifeq ($(CONFIG_ARCH_HOLI), y)
dtbo-y += holi-vidc.dtbo
endif

ifeq ($(CONFIG_ARCH_VIENNA), y)
dtbo-y += vienna-vidc.dtbo
endif

always-y    := $(dtb-y) $(dtbo-y)
subdir-y    := $(dts-dirs)
clean-files    := *.dtb *.dtbo
