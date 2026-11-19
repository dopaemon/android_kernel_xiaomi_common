ifeq ($(CONFIG_ARCH_SUN),y)
dtbo-y += sun-ese-mtp.dtbo
dtbo-y += sun-ese-rcm-v8.dtbo
dtbo-y += sun-ese-rcm.dtbo
dtbo-y += sun-ese-mtp-kiwi-v8.dtbo
dtbo-y += sun-ese-cdp-kiwi-v8.dtbo
dtbo-y += sun-ese-atp.dtbo
dtbo-y += sun-ese-qrd-sku1.dtbo
dtbo-y += sun-ese-qrd-sku1-v8.dtbo
dtbo-y += sun-ese-qrd-sku2-v8.dtbo
dtbo-y += sun-ese-cdp.dtbo
dtbo-y += sun-ese-qrd.dtbo
dtbo-y += sun-v2-ese-mtp.dtbo
dtbo-y += sun-v2-ese-cdp.dtbo
dtbo-y += sun-v2-ese-qrd.dtbo
endif

ifeq ($(CONFIG_ARCH_TUNA),y)
dtbo-y += tuna-ese-cdp.dtbo
dtbo-y += tuna-ese-mtp.dtbo
dtbo-y += tuna-ese-qrd.dtbo
dtbo-y += tuna-ese-mtp-kiwi-overlay.dtbo
dtbo-y += tuna-ese-mtp-kiwi-harmonium-overlay.dtbo
dtbo-y += tuna-ese-rcm-kiwi-overlay.dtbo
dtbo-y += tuna-ese-mtp-qmp1000-overlay.dtbo
dtbo-y += tuna-ese-oemvm-mtp-kiwi.dtbo
dtbo-y += tuna-ese-oemvm-rcm-kiwi.dtbo
endif

ifeq ($(CONFIG_ARCH_KERA),y)
dtbo-y += kera-ese-atp.dtbo
dtbo-y += kera-ese-cdp-qca6750-ufs2.dtbo
dtbo-y += kera-ese-cdp-qca6750-ufs3.dtbo
dtbo-y += kera-ese-cdp-qca6750-ufs4.dtbo
dtbo-y += kera-ese-cdp.dtbo
dtbo-y += kera-ese-mtp-qca6750-qmp1000.dtbo
dtbo-y += kera-ese-mtp-qca6750.dtbo
dtbo-y += kera-ese-mtp-wcn7750-qmp1000.dtbo
dtbo-y += kera-ese-mtp-wcn7750-ufs3.dtbo
dtbo-y += kera-ese-mtp-wcn7750-ufs4.dtbo
dtbo-y += kera-ese-mtp.dtbo
dtbo-y += kera-ese-oemvm-mtp.dtbo
dtbo-y += kera-ese-oemvm-rcm.dtbo
dtbo-y += kera-ese-qrd-wcn7750-ufs2.dtbo
dtbo-y += kera-ese-qrd-wcn7750-ufs3.dtbo
dtbo-y += kera-ese-qrd.dtbo
dtbo-y += kera-ese-rcm-qca6750-ufs2.dtbo
dtbo-y += kera-ese-rcm-qca6750-ufs3.dtbo
dtbo-y += kera-ese-rcm-wcn7750-ufs2.dtbo
dtbo-y += kera-ese-rcm-wcn7750-ufs3.dtbo
dtbo-y += kera-ese-rcm-wcn7750-ufs4.dtbo
dtbo-y += kera-ese-rcm.dtbo
endif

always-y	:= $(dtb-y) $(dtbo-y)
subdir-y	:= $(dts-dirs)
clean-files	:= *.dtb *.dtbo
