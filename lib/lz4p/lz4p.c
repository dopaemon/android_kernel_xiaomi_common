// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (C) 2024, X-Ring technologies Inc., All rights reserved.
 */

#include <linux/init.h>
#include <linux/module.h>
#include <linux/crypto.h>
#include <linux/vmalloc.h>
#include <linux/lz4.h>
#include <crypto/internal/scompress.h>

#include "lz4p.h"

struct lz4_ctx {
	void *lz4_comp_mem;
};

static void *lz4_alloc_ctx(struct crypto_scomp *tfm)
{
	void *ctx;

	ctx = vmalloc(LZ4_MEM_COMPRESS);
	if (!ctx)
		return ERR_PTR(-ENOMEM);

	return ctx;
}

static int lz4_init(struct crypto_tfm *tfm)
{
	struct lz4_ctx *ctx = crypto_tfm_ctx(tfm);

	ctx->lz4_comp_mem = lz4_alloc_ctx(NULL);
	if (IS_ERR(ctx->lz4_comp_mem)) {
		pr_err("lz4p %s err, lz4_comp_mem: %p\n", __func__, ctx->lz4_comp_mem);
		return -ENOMEM;
	}

	return 0;
}

static void lz4_free_ctx(struct crypto_scomp *tfm, void *ctx)
{
	vfree(ctx);
}

static void lz4_exit(struct crypto_tfm *tfm)
{
	struct lz4_ctx *ctx = crypto_tfm_ctx(tfm);

	lz4_free_ctx(NULL, ctx->lz4_comp_mem);
}

static int __lz4_compress_crypto(const u8 *src, unsigned int slen,
				 u8 *dst, unsigned int *dlen, void *ctx)
{
	int out_len = LZ4P_compress_default(src, dst,
		slen, *dlen, ctx);

	if (!out_len) {
		pr_err("lz4_compress_crypto err\n");
		return -EINVAL;
	}

	*dlen = out_len;
	return 0;
}

static int lz4_scompress(struct crypto_scomp *tfm, const u8 *src,
			 unsigned int slen, u8 *dst, unsigned int *dlen,
			 void *ctx)
{
	return __lz4_compress_crypto(src, slen, dst, dlen, ctx);
}

static int lz4_compress_crypto(struct crypto_tfm *tfm, const u8 *src,
			       unsigned int slen, u8 *dst, unsigned int *dlen)
{
	struct lz4_ctx *ctx = crypto_tfm_ctx(tfm);

	return __lz4_compress_crypto(src, slen, dst, dlen, ctx->lz4_comp_mem);
}

static int __lz4_decompress_crypto(const u8 *src, unsigned int slen,
				   u8 *dst, unsigned int *dlen, void *ctx)
{
	int out_len = LZ4P_decompress_safe(src, dst, slen, *dlen, ctx);

	if (out_len < 0) {
		pr_err("lz4_decompress_crypto err\n");
		return -EINVAL;
	}

	*dlen = out_len;
	return 0;
}

static int lz4_sdecompress(struct crypto_scomp *tfm, const u8 *src,
			   unsigned int slen, u8 *dst, unsigned int *dlen,
			   void *ctx)
{
	return __lz4_decompress_crypto(src, slen, dst, dlen, ctx);
}

static int lz4_decompress_crypto(struct crypto_tfm *tfm, const u8 *src,
				 unsigned int slen, u8 *dst,
				 unsigned int *dlen)
{
	return __lz4_decompress_crypto(src, slen, dst, dlen, NULL);
}

static struct crypto_alg alg_lz4 = {
	.cra_name		= "lz4p",
	.cra_driver_name	= "lz4p-generic",
	.cra_flags		= CRYPTO_ALG_TYPE_COMPRESS,
	.cra_ctxsize		= sizeof(struct lz4_ctx),
	.cra_module		= THIS_MODULE,
	.cra_init		= lz4_init,
	.cra_exit		= lz4_exit,
	.cra_u			= { .compress = {
	.coa_compress		= lz4_compress_crypto,
	.coa_decompress		= lz4_decompress_crypto } }
};

static struct scomp_alg scomp = {
	.alloc_ctx		= lz4_alloc_ctx,
	.free_ctx		= lz4_free_ctx,
	.compress		= lz4_scompress,
	.decompress		= lz4_sdecompress,
	.base			= {
		.cra_name	= "lz4p",
		.cra_driver_name = "lz4p-scomp",
		.cra_module	 = THIS_MODULE,
	}
};

int lz4p_init(void)
{
	int ret;

	ret = crypto_register_alg(&alg_lz4);
	if (ret) {
		pr_err("crypto_register_alg err, ret: %d\n", ret);
		return ret;
	}

	ret = crypto_register_scomp(&scomp);
	if (ret) {
		pr_err("crypto_register_scomp err, ret: %d\n", ret);
		crypto_unregister_alg(&alg_lz4);
		return ret;
	}

	return ret;
}

void lz4p_exit(void)
{
	crypto_unregister_alg(&alg_lz4);
	crypto_unregister_scomp(&scomp);
}
