# SPDX-License-Identifier: GPL-2.0-only
ccflags-y += -Wno-declaration-after-statement -Wno-gcc-compat

obj-$(CONFIG_XRING_ZRAM_LZ4P)	:=	lz4p.o

lz4p-y	:= lz4p.o lz4p_compress.o lz4p_decompress.o lz4p_encode.o lz4p_decode.o
