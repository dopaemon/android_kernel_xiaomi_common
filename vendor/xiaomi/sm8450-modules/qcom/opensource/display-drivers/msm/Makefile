# SPDX-License-Identifier: GPL-2.0

KBUILD_OPTIONS+= DISPLAY_ROOT=$(KERNEL_SRC)/$(M)/../
KBUILD_OPTIONS += KBUILD_EXTRA_SYMBOLS+=$(M)/../../mm-drivers/sync_fence/Module.symvers
KBUILD_OPTIONS += KBUILD_EXTRA_SYMBOLS+=$(M)/../../mm-drivers/msm_ext_display/Module.symvers
KBUILD_OPTIONS += KBUILD_EXTRA_SYMBOLS+=$(M)/../../mmrm-driver/Module.symvers
KBUILD_OPTIONS += KBUILD_EXTRA_SYMBOLS+=$(M)/../../securemsm-kernel/Module.symvers
KBUILD_OPTIONS += KBUILD_EXTRA_SYMBOLS+=$(M)/../../synx-kernel/Module.symvers

all: modules

modules_install:
	$(MAKE) INSTALL_MOD_STRIP=1 -C $(KERNEL_SRC) M=$(M) modules_install

%:
	$(MAKE) -C $(KERNEL_SRC) M=$(M) $@ $(KBUILD_OPTIONS)

clean:
	rm -f *.o *.ko *.mod.c *.mod.o *~ .*.cmd Module.symvers
	rm -rf .tmp_versions

