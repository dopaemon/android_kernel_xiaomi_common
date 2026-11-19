ifeq ($(CONFIG_ARCH_SUN),y)
dtbo-y += nxp/sun-nfc.dtbo \
          nxp/sun-nfc-cdp.dtbo \
          nxp/sun-nfc-mtp.dtbo \
          nxp/sun-nfc-qrd.dtbo \
          nxp/tuna-nfc.dtbo \
          nxp/kera-nfc.dtbo \

dtbo-y += st/sun-nfc.dtbo \
          st/sun-nfc-mtp.dtbo \
          st/sun-nfc-cdp.dtbo \
          st/sun-nfc-qrd-sku1.dtbo \
          st/sun-nfc-qrd-sku1-v8.dtbo \
          st/sun-nfc-qrd-sku2-v8.dtbo \
          st/sun-nfc-atp.dtbo \
          st/sun-mtp-kiwi-v8.dtbo \
          st/sun-nfc-rcm.dtbo \
          st/tuna-nfc.dtbo \
          st/kera-nfc.dtbo
endif

ifeq ($(CONFIG_ARCH_PARROT),y)
dtbo-y += nxp/parrot-nfc.dtbo \
          nxp/parrot-nfc-qrd.dtbo \
          nxp/parrot-nfc-idp.dtbo \
          nxp/parrot-nfc-atp.dtbo \
          nxp/parrot-nfc-idp-wcn6755.dtbo \
          nxp/parrot-nfc-idp-wcn3990-amoled-rcm.dtbo
endif

ifeq ($(CONFIG_ARCH_RAVELIN),y)
dtbo-y += nxp/ravelin-nfc.dtbo
dtbo-y += nxp/montague-hsp-idp-nfc.dtbo
endif


always-y        := $(dtb-y) $(dtbo-y)
subdir-y        := $(dts-dirs)
clean-files     := *.dtb *.dtbo
