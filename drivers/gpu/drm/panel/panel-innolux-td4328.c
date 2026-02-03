// SPDX-License-Identifier: GPL-2.0-only
// Copyright (c) 2024 AYN Odin Mainline
// Copyright (c) 2026 REG Linux Team
// Innolux TD4328 1080x1920 LCD panel driver

#include <linux/delay.h>
#include <linux/gpio/consumer.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/regulator/consumer.h>

#include <drm/drm_mipi_dsi.h>
#include <drm/drm_modes.h>
#include <drm/drm_panel.h>

struct innolux_td4328 {
	struct drm_panel panel;
	struct mipi_dsi_device *dsi;
	struct regulator_bulk_data supplies[3];
	struct gpio_desc *reset_gpio;
	struct gpio_desc *enable_gpio;
	enum drm_panel_orientation orientation;
	bool prepared;
};

static inline struct innolux_td4328 *to_innolux_td4328(struct drm_panel *panel)
{
	return container_of(panel, struct innolux_td4328, panel);
}

static void innolux_td4328_reset(struct innolux_td4328 *ctx)
{
	/* Enable panel power if enable GPIO is present */
	if (ctx->enable_gpio) {
		gpiod_set_value_cansleep(ctx->enable_gpio, 1);
		usleep_range(10000, 11000);
	}

	/* Reset sequence: assert, wait, de-assert, wait for ready
	 * Note: With GPIOD_OUT_LOW initial state, the logic is:
	 * - gpiod_set_value(..., 1) = assert reset (panel in reset)
	 * - gpiod_set_value(..., 0) = de-assert reset (panel running)
	 */
	gpiod_set_value_cansleep(ctx->reset_gpio, 1);
	usleep_range(10000, 11000);
	gpiod_set_value_cansleep(ctx->reset_gpio, 0);
	msleep(120);  /* Increased from 80ms for panel stability */
}

static int innolux_td4328_on(struct innolux_td4328 *ctx)
{
	struct mipi_dsi_device *dsi = ctx->dsi;
	struct device *dev = &dsi->dev;
	int ret;

	dsi->mode_flags |= MIPI_DSI_MODE_LPM;

	/* Panel initialization sequence */
	ret = mipi_dsi_dcs_write(dsi, 0xb0, (u8[]){ 0x00 }, 1);
	if (ret < 0) {
		dev_err(dev, "Failed to write B0 command: %d\n", ret);
		return ret;
	}

	static const u8 seq_c2[] = {
		0x01, 0xF7, 0x80, 0x08, 0x68, 0x08, 0x0C,
		0x10, 0x00, 0x08, 0x70, 0x00, 0x00, 0x00,
		0x00, 0x00, 0x00, 0x00, 0x03, 0x83, 0x00,
		0x00, 0x00
	};
	ret = mipi_dsi_dcs_write(dsi, 0xc2, seq_c2, sizeof(seq_c2));
	if (ret < 0) {
		dev_err(dev, "Failed to write C2 command: %d\n", ret);
		return ret;
	}

	ret = mipi_dsi_dcs_write(dsi, 0xd6, (u8[]){ 0x01 }, 1);
	if (ret < 0) {
		dev_err(dev, "Failed to write D6 command: %d\n", ret);
		return ret;
	}

	ret = mipi_dsi_dcs_write(dsi, 0xb0, (u8[]){ 0x03 }, 1);
	if (ret < 0) {
		dev_err(dev, "Failed to write B0 (0x03) command: %d\n", ret);
		return ret;
	}

	/* Set column address */
	static const u8 seq_2a[] = { 0x00, 0x00, 0x04, 0x37 };
	ret = mipi_dsi_dcs_write(dsi, 0x2a, seq_2a, sizeof(seq_2a));
	if (ret < 0) {
		dev_err(dev, "Failed to set column address: %d\n", ret);
		return ret;
	}

	/* Set page address */
	static const u8 seq_2b[] = { 0x00, 0x00, 0x07, 0x7f };
	ret = mipi_dsi_dcs_write(dsi, 0x2b, seq_2b, sizeof(seq_2b));
	if (ret < 0) {
		dev_err(dev, "Failed to set page address: %d\n", ret);
		return ret;
	}

	/* Set tearing effect on */
	ret = mipi_dsi_dcs_set_tear_on(dsi, MIPI_DSI_DCS_TEAR_MODE_VBLANK);
	if (ret < 0) {
		dev_err(dev, "Failed to set tearing effect: %d\n", ret);
		return ret;
	}

	/* Exit sleep mode */
	ret = mipi_dsi_dcs_exit_sleep_mode(dsi);
	if (ret < 0) {
		dev_err(dev, "Failed to exit sleep mode: %d\n", ret);
		return ret;
	}
	msleep(150);

	/* Turn display on */
	ret = mipi_dsi_dcs_set_display_on(dsi);
	if (ret < 0) {
		dev_err(dev, "Failed to set display on: %d\n", ret);
		return ret;
	}
	msleep(50);

	return 0;
}

static int innolux_td4328_off(struct innolux_td4328 *ctx)
{
	struct mipi_dsi_device *dsi = ctx->dsi;
	struct device *dev = &dsi->dev;
	int ret;

	dsi->mode_flags &= ~MIPI_DSI_MODE_LPM;

	ret = mipi_dsi_dcs_set_display_off(dsi);
	if (ret < 0) {
		dev_err(dev, "Failed to set display off: %d\n", ret);
		return ret;
	}
	usleep_range(5000, 6000);

	ret = mipi_dsi_dcs_enter_sleep_mode(dsi);
	if (ret < 0) {
		dev_err(dev, "Failed to enter sleep mode: %d\n", ret);
		return ret;
	}
	msleep(83);

	return 0;
}

static int innolux_td4328_prepare(struct drm_panel *panel)
{
	struct innolux_td4328 *ctx = to_innolux_td4328(panel);
	struct device *dev = &ctx->dsi->dev;
	int ret;

	if (ctx->prepared)
		return 0;

	ret = regulator_bulk_enable(ARRAY_SIZE(ctx->supplies), ctx->supplies);
	if (ret < 0) {
		dev_err(dev, "Failed to enable regulators: %d\n", ret);
		return ret;
	}

	innolux_td4328_reset(ctx);

	ret = innolux_td4328_on(ctx);
	if (ret < 0) {
		dev_err(dev, "Failed to initialize panel: %d\n", ret);
		gpiod_set_value_cansleep(ctx->reset_gpio, 1);
		if (ctx->enable_gpio)
			gpiod_set_value_cansleep(ctx->enable_gpio, 0);
		regulator_bulk_disable(ARRAY_SIZE(ctx->supplies), ctx->supplies);
		return ret;
	}

	ctx->prepared = true;
	return 0;
}

static int innolux_td4328_unprepare(struct drm_panel *panel)
{
	struct innolux_td4328 *ctx = to_innolux_td4328(panel);
	struct device *dev = &ctx->dsi->dev;
	int ret;

	if (!ctx->prepared)
		return 0;

	ret = innolux_td4328_off(ctx);
	if (ret < 0)
		dev_err(dev, "Failed to un-initialize panel: %d\n", ret);

	gpiod_set_value_cansleep(ctx->reset_gpio, 1);
	if (ctx->enable_gpio)
		gpiod_set_value_cansleep(ctx->enable_gpio, 0);
	regulator_bulk_disable(ARRAY_SIZE(ctx->supplies), ctx->supplies);

	ctx->prepared = false;
	return 0;
}

static const struct drm_display_mode innolux_td4328_mode = {
	.clock = (1080 + 60 + 10 + 60) * (1920 + 20 + 8 + 20) * 60 / 1000,
	.hdisplay = 1080,
	.hsync_start = 1080 + 60,
	.hsync_end = 1080 + 60 + 10,
	.htotal = 1080 + 60 + 10 + 60,
	.vdisplay = 1920,
	.vsync_start = 1920 + 20,
	.vsync_end = 1920 + 20 + 8,
	.vtotal = 1920 + 20 + 8 + 20,
	.width_mm = 75,
	.height_mm = 132,
};

static int innolux_td4328_get_modes(struct drm_panel *panel,
				    struct drm_connector *connector)
{
	struct innolux_td4328 *ctx = to_innolux_td4328(panel);
	struct drm_display_mode *mode;

	mode = drm_mode_duplicate(connector->dev, &innolux_td4328_mode);
	if (!mode)
		return -ENOMEM;

	drm_mode_set_name(mode);

	mode->type = DRM_MODE_TYPE_DRIVER | DRM_MODE_TYPE_PREFERRED;
	connector->display_info.width_mm = mode->width_mm;
	connector->display_info.height_mm = mode->height_mm;
	drm_mode_probed_add(connector, mode);

	drm_connector_set_panel_orientation(connector, ctx->orientation);

	return 1;
}

static enum drm_panel_orientation innolux_td4328_get_orientation(struct drm_panel *panel)
{
	struct innolux_td4328 *ctx = to_innolux_td4328(panel);

	return ctx->orientation;
}

static const struct drm_panel_funcs innolux_td4328_panel_funcs = {
	.prepare = innolux_td4328_prepare,
	.unprepare = innolux_td4328_unprepare,
	.get_modes = innolux_td4328_get_modes,
	.get_orientation = innolux_td4328_get_orientation,
};

static int innolux_td4328_probe(struct mipi_dsi_device *dsi)
{
	struct device *dev = &dsi->dev;
	struct innolux_td4328 *ctx;
	int ret;

	ctx = devm_kzalloc(dev, sizeof(*ctx), GFP_KERNEL);
	if (!ctx)
		return -ENOMEM;

	/* Match regulator names with device tree:
	 * DT uses: vddio-supply, vsp-supply, vsn-supply
	 */
	ctx->supplies[0].supply = "vddio";  /* Digital I/O - 1.8V */
	ctx->supplies[1].supply = "vsp";    /* Positive supply (LAB) - 5.5V */
	ctx->supplies[2].supply = "vsn";    /* Negative supply (IBB) - -5.5V */
	ret = devm_regulator_bulk_get(dev, ARRAY_SIZE(ctx->supplies),
				      ctx->supplies);
	if (ret < 0)
		return dev_err_probe(dev, ret, "Failed to get regulators\n");

	/* Get reset GPIO - start with it de-asserted (LOW)
	 * DT has: reset-gpios = <&tlmm 6 GPIO_ACTIVE_LOW>
	 * This means reset is asserted when GPIO is LOW
	 * So GPIOD_OUT_LOW means panel NOT in reset (running)
	 */
	ctx->reset_gpio = devm_gpiod_get(dev, "reset", GPIOD_OUT_LOW);
	if (IS_ERR(ctx->reset_gpio))
		return dev_err_probe(dev, PTR_ERR(ctx->reset_gpio),
				     "Failed to get reset-gpios\n");

	/* Get enable GPIO - start disabled
	 * DT has: enable-gpios = <&tlmm 120 GPIO_ACTIVE_HIGH>
	 * This means panel enabled when GPIO is HIGH
	 */
	ctx->enable_gpio = devm_gpiod_get_optional(dev, "enable", GPIOD_OUT_LOW);
	if (IS_ERR(ctx->enable_gpio))
		return dev_err_probe(dev, PTR_ERR(ctx->enable_gpio),
				     "Failed to get enable-gpios\n");

	ret = of_drm_get_panel_orientation(dev->of_node, &ctx->orientation);
	if (ret < 0) {
		dev_err(dev, "Failed to get orientation %d\n", ret);
		return ret;
	}

	ctx->dsi = dsi;
	mipi_dsi_set_drvdata(dsi, ctx);

	dsi->lanes = 4;
	dsi->format = MIPI_DSI_FMT_RGB888;
	dsi->mode_flags = MIPI_DSI_MODE_VIDEO | MIPI_DSI_MODE_VIDEO_SYNC_PULSE |
			  MIPI_DSI_MODE_NO_EOT_PACKET |
			  MIPI_DSI_CLOCK_NON_CONTINUOUS;

	drm_panel_init(&ctx->panel, dev, &innolux_td4328_panel_funcs,
		       DRM_MODE_CONNECTOR_DSI);
	ctx->panel.prepare_prev_first = true;

	ret = drm_panel_of_backlight(&ctx->panel);
	if (ret)
		return dev_err_probe(dev, ret, "Failed to get backlight\n");

	drm_panel_add(&ctx->panel);

	ret = mipi_dsi_attach(dsi);
	if (ret < 0) {
		dev_err(dev, "Failed to attach to DSI host: %d\n", ret);
		drm_panel_remove(&ctx->panel);
		return ret;
	}

	return 0;
}

static void innolux_td4328_remove(struct mipi_dsi_device *dsi)
{
	struct innolux_td4328 *ctx = mipi_dsi_get_drvdata(dsi);
	int ret;

	ret = mipi_dsi_detach(dsi);
	if (ret < 0)
		dev_err(&dsi->dev, "Failed to detach from DSI host: %d\n", ret);

	drm_panel_remove(&ctx->panel);
}

static const struct of_device_id innolux_td4328_of_match[] = {
	{ .compatible = "innolux,td4328" },
	{ /* sentinel */ }
};
MODULE_DEVICE_TABLE(of, innolux_td4328_of_match);

static struct mipi_dsi_driver innolux_td4328_driver = {
	.probe = innolux_td4328_probe,
	.remove = innolux_td4328_remove,
	.driver = {
		.name = "panel-innolux-td4328",
		.of_match_table = innolux_td4328_of_match,
	},
};
module_mipi_dsi_driver(innolux_td4328_driver);

MODULE_AUTHOR("AYN Odin Mainline Project");
MODULE_DESCRIPTION("Innolux TD4328 1080x1920 LCD panel driver");
MODULE_LICENSE("GPL");
