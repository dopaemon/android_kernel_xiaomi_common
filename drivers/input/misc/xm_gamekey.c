// SPDX-License-Identifier: GPL-2.0
#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/of.h>
#include <linux/of_gpio.h>
#include <linux/gpio/consumer.h>
#include <linux/interrupt.h>
#include <linux/regulator/consumer.h>
#include <linux/input.h>
#include <linux/workqueue.h>
#include <linux/pinctrl/consumer.h>

struct gk_button;

struct xm_gamekey {
	struct device *dev;
	struct regulator *vdd;
	struct input_dev *input;
	struct pinctrl *pct;
	struct pinctrl_state *st_active, *st_suspend;
	unsigned int debounce_ms;

	struct gk_button *btn[4];
};

enum gk_id { GK_KEY_L, GK_KEY_R, GK_HALL_L, GK_HALL_R };

struct gk_button {
	const char *name;
	enum gk_id id;
	struct gpio_desc *gpiod;
	int irq;
	struct delayed_work dwork;
	struct xm_gamekey *parent;
	bool active_low;
};

static void gk_report(struct gk_button *b)
{
	int val = gpiod_get_value_cansleep(b->gpiod);
	if (val < 0)
		return;

	if (b->active_low)
		val = !val;

	dev_dbg(b->parent->dev, "[%d] %s = %d | %d\n", b->id, b->name, val, b->active_low);

	if (b->id > GK_KEY_R) {
		int code;

		switch (b->id) {
			case GK_HALL_L:
				code = val ? KEY_F4 : KEY_F3;
				break;
			case GK_HALL_R:
				code = val ? KEY_F6 : KEY_F5;
				break;
			default:
				code = 0;
				break;
		}

		input_report_key(b->parent->input, code, 1);
		input_sync(b->parent->input);

		input_report_key(b->parent->input, code, 0);
		input_sync(b->parent->input);
	} else {
		int code;

		switch (b->id) {
			case GK_KEY_L:
				code = KEY_F1;
				break;
			case GK_KEY_R:
				code = KEY_F2;
				break;
			default:
				code = 0;
				break;
		}
		
		input_report_key(b->parent->input, code, !val);
		input_sync(b->parent->input);
	}

}

static void gk_work(struct work_struct *ws)
{
	struct delayed_work *dw = to_delayed_work(ws);
	struct gk_button *b = container_of(dw, struct gk_button, dwork);
	gk_report(b);
}

static irqreturn_t gk_irq(int irq, void *data)
{
	struct gk_button *b = data;
	mod_delayed_work(system_wq, &b->dwork, msecs_to_jiffies(b->parent->debounce_ms));
	return IRQ_HANDLED;
}

static struct gpio_desc *gk_get_gpiod(struct device *dev,
				      const char *newname, const char *oldname,
				      enum gpiod_flags flags)
{
	struct gpio_desc *d;

	d = devm_gpiod_get_optional(dev, newname, flags);
	if (!IS_ERR_OR_NULL(d))
		return d;

	if (IS_ERR(d) && PTR_ERR(d) != -ENOENT)
		return d;

	return devm_gpiod_get_from_of_node(dev, dev->of_node, oldname, 0, flags, newname);
}

static int xm_gamekey_probe(struct platform_device *pdev)
{
	int ret, i;
	struct xm_gamekey *ctx;
	static const struct { const char *newp; const char *oldp; enum gk_id id; } map[] = {
		{ "key-left",  "key_left",  GK_KEY_L  },
		{ "key-right", "key_right", GK_KEY_R  },
		{ "hall-left", "hall_left", GK_HALL_L },
		{ "hall-right","hall_right",GK_HALL_R },
	};

	ctx = devm_kzalloc(&pdev->dev, sizeof(*ctx), GFP_KERNEL);
	if (!ctx) return -ENOMEM;
	ctx->dev = &pdev->dev;

	of_property_read_u32(pdev->dev.of_node, "debounce_time", &ctx->debounce_ms);
	if (!ctx->debounce_ms) ctx->debounce_ms = 5;

	ctx->pct = devm_pinctrl_get(&pdev->dev);
	if (!IS_ERR(ctx->pct)) {
		ctx->st_active  = pinctrl_lookup_state(ctx->pct, "gamekey_active");
		ctx->st_suspend = pinctrl_lookup_state(ctx->pct, "gamekey_suspend");
		if (!IS_ERR(ctx->st_active))
			pinctrl_select_state(ctx->pct, ctx->st_active);
	}

	ctx->vdd = devm_regulator_get(&pdev->dev, "gamekey_vreg");
	if (IS_ERR(ctx->vdd)) {
		dev_err(&pdev->dev, "regulator gamekey_vreg not found: %ld\n", PTR_ERR(ctx->vdd));
		return PTR_ERR(ctx->vdd);
	}
	ret = regulator_enable(ctx->vdd);
	if (ret) return ret;

	ctx->input = devm_input_allocate_device(&pdev->dev);
	if (!ctx->input) { ret = -ENOMEM; goto disable_vreg; }
	ctx->input->name = "xm_gamekey";
	ctx->input->phys = "xm_gamekey/input0";
	ctx->input->id.bustype = BUS_HOST;

	for (i = 0; i < 4; i++) {
		struct gk_button *b = devm_kzalloc(&pdev->dev, sizeof(*b), GFP_KERNEL);
		if (!b) { ret = -ENOMEM; goto disable_vreg; }
		ctx->btn[i] = b;
		b->parent = ctx;
		b->id = map[i].id;
		b->name = map[i].newp;
		b->gpiod = gk_get_gpiod(&pdev->dev, map[i].newp, map[i].oldp, GPIOD_IN);
		if (IS_ERR_OR_NULL(b->gpiod)) { ret = b->gpiod ? PTR_ERR(b->gpiod) : -ENODEV; goto disable_vreg; }
		b->active_low = gpiod_is_active_low(b->gpiod);

		switch (map[i].id){
			case GK_KEY_L: {
				input_set_capability(ctx->input, EV_KEY, KEY_F1);
				break;
			}
			case GK_KEY_R: {
				input_set_capability(ctx->input, EV_KEY, KEY_F2);
				break;
			}
			case GK_HALL_L: {
				input_set_capability(ctx->input, EV_KEY, KEY_F4);
				input_set_capability(ctx->input, EV_KEY, KEY_F3);
				break;
			}
			case GK_HALL_R: {
				input_set_capability(ctx->input, EV_KEY, KEY_F5);
				input_set_capability(ctx->input, EV_KEY, KEY_F6);
				break;
			}
		}

		INIT_DELAYED_WORK(&b->dwork, gk_work);
		b->irq = gpiod_to_irq(b->gpiod);
		if (b->irq < 0) { ret = b->irq; goto disable_vreg; }

		ret = devm_request_threaded_irq(&pdev->dev, b->irq, NULL, gk_irq,
				IRQF_TRIGGER_RISING | IRQF_TRIGGER_FALLING | IRQF_ONESHOT,
				b->name, b);
		if (ret) goto disable_vreg;

		gk_report(b);
	}

	ret = input_register_device(ctx->input);
	if (ret) goto disable_vreg;

	platform_set_drvdata(pdev, ctx);
	dev_info(&pdev->dev, "xm_gamekey probed, debounce=%u ms\n", ctx->debounce_ms);
	return 0;

disable_vreg:
	regulator_disable(ctx->vdd);
	return ret;
}

static int xm_gamekey_remove(struct platform_device *pdev)
{
	int i;
	struct xm_gamekey *ctx = platform_get_drvdata(pdev);

	for (i = 0; i < 4; i++)
		if (ctx->btn[i])
			cancel_delayed_work_sync(&ctx->btn[i]->dwork);

	if (ctx->st_suspend && !IS_ERR(ctx->pct))
		pinctrl_select_state(ctx->pct, ctx->st_suspend);

	if (!IS_ERR_OR_NULL(ctx->vdd))
		regulator_disable(ctx->vdd);

	return 0;
}

static const struct of_device_id xm_gamekey_of_match[] = {
	{ .compatible = "xm,gamekey" },
	{ }
};
MODULE_DEVICE_TABLE(of, xm_gamekey_of_match);

static struct platform_driver xm_gamekey_driver = {
	.driver = {
		.name = "xm_gamekey",
		.of_match_table = xm_gamekey_of_match,
	},
	.probe  = xm_gamekey_probe,
	.remove = xm_gamekey_remove,
};
module_platform_driver(xm_gamekey_driver);

MODULE_LICENSE("GPL v2");
MODULE_DESCRIPTION("Xiaomi gamekey buttons support");
MODULE_AUTHOR("n08i40k");

