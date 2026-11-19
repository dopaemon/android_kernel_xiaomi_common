KERNEL_SRC ?= /lib/modules/$(shell uname -r)/build
M ?= $(shell pwd)

M=$(PWD)
BT_ROOT=$(KERNEL_SRC)/$(M)

KBUILD_OPTIONS+=  BT_ROOT=$(BT_ROOT)
KBUILD_OPTIONS += KBUILD_EXTRA_SYMBOLS=$(M)/../audio-kernel/Module.symvers
KBUILD_OPTIONS += KBUILD_EXTRA_SYMBOLS+=$(M)/../wlan/platform/Module.symvers

all:
	$(MAKE) -C $(KERNEL_SRC) M=$(M) modules $(KBUILD_OPTIONS)

modules_install:
	$(MAKE) INSTALL_MOD_STRIP=1 -C $(KERNEL_SRC) M=$(M) modules_install

clean:
	$(MAKE) -C $(KERNEL_SRC) M=$(M) clean
