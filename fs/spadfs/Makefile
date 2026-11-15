SPADFS_NAME = spadfs
CFLAGS_super.o := -DSPADFS_NAME=$(SPADFS_NAME)
obj-$(CONFIG_SPADFS_FS) += $(SPADFS_NAME).o
$(SPADFS_NAME)-y := alloc.o allocmem.o buffer.o bufstruc.o dir.o file.o inode.o ioctl.o link.o name.o namei.o super.o xattr.o
