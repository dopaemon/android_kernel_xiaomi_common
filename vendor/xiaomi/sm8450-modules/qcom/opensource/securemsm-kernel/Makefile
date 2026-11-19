M=$(PWD)
SSG_MODULE_ROOT=$(KERNEL_SRC)/$(M)
INC=-I/$(M)/linux/*
KBUILD_OPTIONS+=SSG_MODULE_ROOT=$(SSG_MODULE_ROOT)
KBUILD_OPTIONS+=CONFIG_DISABLE_TEST_MODULES=y

all: modules

clean:
	rm -f *.cmd *.d *.mod *.o *.ko *.mod.c *.mod.o Module.symvers modules.order

%:
	$(MAKE) -C $(KERNEL_SRC) M=$(M) $(INC) $@ $(KBUILD_OPTIONS)