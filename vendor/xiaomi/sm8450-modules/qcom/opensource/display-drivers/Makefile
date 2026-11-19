# SPDX-License-Identifier: GPL-2.0-only
M=$(PWD)
DISPLAY_ROOT=$(KERNEL_SRC)/$(M)
KBUILD_OPTIONS := DISPLAY_ROOT=$(DISPLAY_ROOT)

ifeq ($(TARGET_SUPPORT),genericarmv8)
	KBUILD_OPTIONS += CONFIG_ARCH_PARROT=y
endif

obj-m += msm/

all:
	$(MAKE) -C $(KERNEL_SRC) M=$(M) modules $(KBUILD_OPTIONS)

modules_install:
	$(MAKE) INSTALL_MOD_STRIP=1 -C $(KERNEL_SRC) M=$(M) modules_install

%:
	$(MAKE) -C $(KERNEL_SRC) M=$(M) $@ $(KBUILD_OPTIONS)

clean:
	rm -f *.o *.ko *.mod.c *.mod.o *~ .*.cmd Module.symvers
	rm -rf .tmp_versions
