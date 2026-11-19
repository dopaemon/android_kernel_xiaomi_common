ifneq ($(CONFIG_ARCH_QTI_VM), y)
dtbo-$(CONFIG_ARCH_PINEAPPLE) += display/pineapple-sde.dtbo \
		display/pineapple-sde-display-rumi-overlay.dtbo \
		display/pineapple-sde-display-cdp-overlay.dtbo \
		display/pineapple-sde-display-rcm-overlay.dtbo \
		display/pineapple-sde-display-mtp-overlay.dtbo \
		display/pineapple-sde-display-qrd-overlay.dtbo \
		display/pineapple-sde-display-cdp-nfc-overlay.dtbo \
		display/pineapple-sde-display-mtp-nfc-overlay.dtbo \
		display/pineapple-sde-display-atp-overlay.dtbo
else
dtbo-$(CONFIG_ARCH_PINEAPPLE) += display/trustedvm-pineapple-sde-display-mtp-overlay.dtbo \
		  display/trustedvm-pineapple-sde-display-cdp-overlay.dtbo \
		  display/trustedvm-pineapple-sde-display-rumi-overlay.dtbo \
		  display/trustedvm-pineapple-sde-display-qrd-overlay.dtbo \
		  display/trustedvm-pineapple-sde-display-atp-overlay.dtbo
endif

ifneq ($(CONFIG_ARCH_QTI_VM), y)
dtbo-$(CONFIG_ARCH_BLAIR) += display/blair-sde.dtbo \
		display/blair-sde-display-cdp-overlay.dtbo \
		display/blair-sde-display-mtp-overlay.dtbo \
		display/blair-sde-display-qrd-overlay.dtbo \
		display/blair-sde-display-atp-overlay.dtbo
endif

ifneq ($(CONFIG_ARCH_QTI_VM), y)
dtbo-$(CONFIG_ARCH_PARROT) += display/parrot-sde.dtbo \
		display/parrot-sde-display-atp-overlay.dtbo \
		display/parrot-sde-display-idp-overlay.dtbo \
		display/parrot-sde-display-idp-amoled-overlay.dtbo \
		display/parrot-sde-display-rumi-overlay.dtbo \
		display/parrot-sde-display-qrd-overlay.dtbo
else
dtbo-$(CONFIG_ARCH_PARROT) += display/trustedvm-parrot-sde-display-idp-overlay.dtbo \
		display/trustedvm-parrot-sde-display-qrd-overlay.dtbo
endif

ifneq ($(CONFIG_ARCH_QTI_VM), y)
dtbo-$(CONFIG_ARCH_HOLI) += display/holi-sde.dtbo \
		display/holi-sde-display-cdp-overlay.dtbo \
		display/holi-sde-display-cdp-lcd-overlay.dtbo \
		display/holi-sde-display-mtp-overlay.dtbo \
		display/holi-sde-display-qrd-overlay.dtbo \
		display/holi-sde-display-atp-overlay.dtbo \
		display/holi-sde-display-mtp-pm6125-overlay.dtbo \
		display/holi-sde-display-qrd-pm6125-overlay.dtbo \
		display/holi-sde-display-cdp-pm6125-overlay.dtbo
endif

ifneq ($(CONFIG_ARCH_QTI_VM), y)
dtbo-$(CONFIG_ARCH_RAVELIN) += display/ravelin-sde.dtbo \
                display/ravelin-sde-display-atp-overlay.dtbo \
                display/ravelin-sde-display-idp-overlay.dtbo \
                display/ravelin-sde-display-idp-amoled-overlay.dtbo \
                display/ravelin-sde-display-rumi-overlay.dtbo \
                display/ravelin-sde-display-qrd-overlay.dtbo
else
dtbo-$(CONFIG_ARCH_RAVELIN) += display/trustedvm-ravelin-sde-display-idp-overlay.dtbo \
                display/trustedvm-ravelin-sde-display-idp-amoled-overlay.dtbo
endif


ifneq ($(CONFIG_ARCH_QTI_VM), y)
dtbo-$(CONFIG_ARCH_KALAMA) += display/kalama-sde.dtbo \
		display/kalama-sde-display-rumi-overlay.dtbo \
		display/kalama-sde-display-cdp-overlay.dtbo \
		display/kalama-sde-display-cdp-wsa883x-overlay.dtbo \
		display/kalama-sde-display-mtp-overlay.dtbo \
		display/kalama-sde-display-qrd-overlay.dtbo \
		display/kalama-sde-display-hdk-overlay.dtbo \
		display/kalama-sde-display-hhg-overlay.dtbo \
		display/kalama-sde-display-mtp-nfc-overlay.dtbo \
		display/kalama-sde-display-rcm-overlay.dtbo \
		display/kalama-sde-display-cdp-nfc-overlay.dtbo \
		display/kalama-sde-display-atp-overlay.dtbo
else
dtbo-$(CONFIG_ARCH_KALAMA) += display/trustedvm-kalama-sde-display-mtp-overlay.dtbo \
                  display/trustedvm-kalama-sde-display-mtp-nfc-overlay.dtbo \
                  display/trustedvm-kalama-sde-display-cdp-nfc-overlay.dtbo \
		  display/trustedvm-kalama-sde-display-cdp-overlay.dtbo \
		  display/trustedvm-kalama-sde-display-cdp-wsa883x-overlay.dtbo \
		  display/trustedvm-kalama-sde-display-rumi-overlay.dtbo \
		  display/trustedvm-kalama-sde-display-qrd-overlay.dtbo \
		  display/trustedvm-kalama-sde-display-atp-overlay.dtbo
endif

ifeq ($(CONFIG_ARCH_SA8155), y)
dtbo-y += display/sa8155-adp-star-display.dtbo
endif

always-y    := $(dtb-y) $(dtbo-y)
subdir-y    := $(dts-dirs)
clean-files    := *.dtb *.dtbo
