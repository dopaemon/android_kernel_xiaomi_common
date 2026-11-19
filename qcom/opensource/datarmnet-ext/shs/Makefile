KERNEL_SRC ?= /lib/modules/$(shell uname -r)/build

DATARMNET_CORE_PATH := datarmnet/core
KBUILD_OPTIONS := RMNET_SHS_ROOT=$(PWD)
KBUILD_OPTIONS += MODNAME?=rmnet_shs
KBUILD_OPTIONS += RMNET_CORE_INC_DIR=$(KERNEL_SRC)/$(M)/../../$(DATARMNET_CORE_PATH)
KBUILD_EXTRA_SYMBOLS := $(M)/../../$(DATARMNET_CORE_PATH)/Module.symvers
KBUILD_OPTIONS += KBUILD_EXTRA_SYMBOLS=$(KBUILD_EXTRA_SYMBOLS)
M ?= $(shell pwd)

all:
	$(MAKE) -C $(KERNEL_SRC) M=$(M) modules $(KBUILD_OPTIONS) $(KBUILD_EXTRA_SYMBOLS)

modules_install:
	$(MAKE) INSTALL_MOD_STRIP=1 -C $(KERNEL_SRC) M=$(M) modules_install

clean:
	$(MAKE) -C $(KERNEL_SRC) M=$(M) clean
