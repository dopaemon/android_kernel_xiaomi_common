# Makefile for use with Android's kernel/build system

KBUILD_OPTIONS += CAMERA_KERNEL_ROOT=$(shell pwd)
KBUILD_OPTIONS += KERNEL_ROOT=$(KERNEL_SRC)
KBUILD_OPTIONS += MODNAME=camera
KBUILD_OPTIONS += KBUILD_EXTRA_SYMBOLS+=$(M)/../mmrm-driver/Module.symvers
KBUILD_OPTIONS += KBUILD_EXTRA_SYMBOLS+=$(M)/../securemsm-kernel/Module.symvers

all: modules

modules dtbs:
	$(MAKE) -C $(KERNEL_SRC) M=$(M) modules $(KBUILD_OPTIONS)

modules_install:
	$(MAKE) M=$(M) -C $(KERNEL_SRC) modules_install

clean:
	$(MAKE) -C $(KERNEL_SRC) M=$(M) clean
