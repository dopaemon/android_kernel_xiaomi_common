dtbo-y := synx/sun-synx.dtbo
ifneq (, $(filter y, $(CONFIG_ARCH_TUNA) $(CONFIG_ARCH_KERA)))
	dtbo-y += synx/tuna-synx.dtbo
endif
#dtbo-y += sun-synx-cdp.dtbo
#dtbo-y += sun-synx-mtp.dtbo
#dtbo-y += sun-synx-qrd.dtbo
