ifeq ($(CONFIG_ARCH_WAIPIO), y)
dtbo-y += gpu/waipio-gpu.dtbo \
		gpu/waipio-v2-gpu.dtbo
endif

ifeq ($(CONFIG_ARCH_KALAMA), y)
dtbo-y += gpu/kalama-gpu.dtbo \
		gpu/kalama-v2-gpu.dtbo \
		gpu/kalama-iot-gpu.dtbo \
		gpu/kalamap-hhg-gpu.dtbo
endif

ifeq ($(CONFIG_ARCH_PINEAPPLE), y)
dtbo-y += gpu/pineapple-gpu.dtbo \
		gpu/pineapple-v2-gpu.dtbo
endif

ifeq ($(CONFIG_ARCH_SA8155), y)
dtbo-y += gpu/sa8155-v2-gpu.dtbo
endif

ifeq ($(CONFIG_ARCH_KHAJE), y)
dtbo-y += gpu/khaje-gpu.dtbo \
		gpu/khajep-gpu.dtbo \
		gpu/khajeq-gpu.dtbo \
		gpu/khajeg-gpu.dtbo
endif

ifeq ($(CONFIG_ARCH_SA8195), y)
dtbo-y += gpu/sa8195p-gpu.dtbo
endif

ifeq ($(CONFIG_ARCH_SA6155), y)
dtbo-y += gpu/sa6155p-gpu.dtbo
endif

ifeq ($(CONFIG_ARCH_MONACO), y)
dtbo-y += gpu/monaco-gpu.dtbo
endif

ifeq ($(CONFIG_ARCH_LEMANS), y)
dtbo-y += gpu/lemans-gpu.dtbo \
		gpu/lemans-gpu-ivi-adas-star.dtbo
endif

ifeq ($(CONFIG_ARCH_KONA), y)
dtbo-y += gpu/kona-gpu.dtbo \
		gpu/kona-v2-gpu.dtbo \
		gpu/kona-v2.1-gpu.dtbo
endif

ifeq ($(CONFIG_ARCH_BLAIR), y)
dtbo-y += gpu/blair-gpu.dtbo
endif

ifeq ($(CONFIG_ARCH_TRINKET), y)
dtbo-y += gpu/trinket-gpu.dtbo \
		gpu/trinketp-gpu.dtbo
endif

ifeq ($(CONFIG_ARCH_HOLI), y)
dtbo-y += gpu/holi-gpu.dtbo
endif

ifeq ($(CONFIG_ARCH_ANORAK), y)
dtbo-y += gpu/anorak-gpu.dtbo
endif

ifeq ($(CONFIG_ARCH_NEO), y)
dtbo-y += gpu/neo-gpu.dtbo \
		gpu/neo-luna-v2-gpu.dtbo
endif

ifeq ($(CONFIG_ARCH_QCS405), y)
dtbo-y += gpu/qcs405-gpu.dtbo
endif

ifeq ($(CONFIG_ARCH_NIOBE), y)
dtbo-y += gpu/niobe-gpu.dtbo
endif

ifeq ($(CONFIG_ARCH_PITTI), y)
dtbo-y += gpu/pitti-gpu.dtbo
endif

ifeq ($(CONFIG_ARCH_CLIFFS), y)
dtbo-y += gpu/cliffs-gpu.dtbo \
		gpu/cliffs7-gpu.dtbo
endif

ifeq ($(CONFIG_ARCH_BENGAL), y)
dtbo-y += gpu/bengal-gpu.dtbo
endif

ifeq ($(CONFIG_ARCH_SDM670), y)
dtbo-y += gpu/qcs605-gpu.dtbo
endif

ifeq ($(CONFIG_ARCH_PARROT), y)
dtbo-y += gpu/parrot-gpu.dtbo \
		gpu/parrot-sg-gpu.dtbo \
		gpu/parrotp-sg-gpu.dtbo
endif

ifeq ($(CONFIG_ARCH_RAVELIN), y)
dtbo-y += gpu/ravelin-gpu.dtbo \
		gpu/ravelin-sg-gpu.dtbo \
		gpu/ravelinp-sg-gpu.dtbo
endif

ifeq ($(CONFIG_ARCH_SCUBA), y)
dtbo-y += gpu/scuba-gpu.dtbo
endif

ifeq ($(CONFIG_ARCH_SM6150), y)
dtbo-y += gpu/sm6150-gpu.dtbo
endif

always-y    := $(dtb-y) $(dtbo-y)
subdir-y    := $(dts-dirs)
clean-files    := *.dtb *.dtbo
