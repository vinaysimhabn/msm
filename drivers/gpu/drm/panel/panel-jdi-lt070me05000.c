/*
 * Copyright (C) 2016 InforceComputing
 * Author: Vinay Simha BN <simhavcs@gmail.com>
 *
 * Copyright (C) 2016 Linaro Ltd
 * Author: Sumit Semwal <sumit.semwal@linaro.org>
 *
 * From internet archives, the panel for Nexus 7 2nd Gen, 2013 model is a
 * JDI model LT070ME05000, and its data sheet is at:
 * http://panelone.net/en/7-0-inch/JDI_LT070ME05000_7.0_inch-datasheet
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License version 2 as published by
 * the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License for
 * more details.
 *
 * You should have received a copy of the GNU General Public License along with
 * this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#include <linux/gpio/consumer.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/regulator/consumer.h>

#include <drm/drmP.h>
#include <drm/drm_crtc.h>
#include <drm/drm_mipi_dsi.h>
#include <drm/drm_panel.h>

#include <video/mipi_display.h>

#define PANEL_DEV_REGULATOR_MAX   3
#define PANEL_MAX_BRIGHTNESS	0xFF

const char *regs[] = {
	"power",
	"vddp",
	"dcdc_en"
};

struct jdi_panel {
	struct drm_panel base;
	struct mipi_dsi_device *dsi;

	struct regulator_bulk_data supplies[PANEL_DEV_REGULATOR_MAX];

	struct gpio_desc *reset_gpio;
	struct gpio_desc *enable_gpio;
	struct gpio_desc *vcc_gpio;
	struct gpio_desc *pwm_gpio;
	struct backlight_device *backlight;

	bool prepared;
	bool enabled;

	const struct drm_display_mode *mode;
};

static inline struct jdi_panel *to_jdi_panel(struct drm_panel *panel)
{
	return container_of(panel, struct jdi_panel, base);
}

static int jdi_panel_init(struct jdi_panel *jdi)
{
	struct mipi_dsi_device *dsi = jdi->dsi;
	int ret;

	dsi->mode_flags |= MIPI_DSI_MODE_LPM;

	ret = mipi_dsi_dcs_soft_reset(dsi);
	if (ret < 0)
		return ret;

	usleep_range(10000, 20000);

	ret = mipi_dsi_dcs_set_pixel_format(dsi, 0x70);
	if (ret < 0)
		return ret;

	ret = mipi_dsi_dcs_set_column_address(dsi, 0x0000, 0x04AF);
	if (ret < 0)
		return ret;

	ret = mipi_dsi_dcs_set_page_address(dsi, 0x0000, 0x077F);
	if (ret < 0)
		return ret;

	ret = mipi_dsi_dcs_set_tear_on(dsi,
				       MIPI_DSI_DCS_TEAR_MODE_VBLANK);
	if (ret < 0)
		return ret;
	usleep_range(5000, 10000);

	ret = mipi_dsi_set_tear_scanline(dsi, 0x03, 0x00);
	if (ret < 0)
		return ret;

	ret = mipi_dsi_dcs_write(dsi, MIPI_DCS_SET_DISPLAY_BRIGHTNESS,
				 (u8[]){ 0xFF }, 1);
	if (ret < 0)
		return ret;

	ret = mipi_dsi_dcs_write(dsi, MIPI_DCS_WRITE_CONTROL_DISPLAY,
				 (u8[]){ 0x24 }, 1);
	if (ret < 0)
		return ret;

	ret = mipi_dsi_dcs_write(dsi, MIPI_DCS_WRITE_POWER_SAVE,
				 (u8[]){ 0x00 }, 1);
	if (ret < 0)
		return ret;

	ret = mipi_dsi_dcs_exit_sleep_mode(dsi);
	if (ret < 0)
		return ret;
	mdelay(120);

	ret = mipi_dsi_generic_write(dsi, (u8[]){0xB0, 0x00}, 2);
	if (ret < 0)
		return ret;
	mdelay(10);

	ret = mipi_dsi_generic_write(dsi, (u8[])
				     {0xB3, 0x26, 0x08, 0x00, 0x20, 0x00}, 6);
	if (ret < 0)
		return ret;
	mdelay(20);

	ret = mipi_dsi_generic_write(dsi, (u8[]){0xB0, 0x03}, 2);
	if (ret < 0)
		return ret;

	return 0;
}

static int jdi_panel_on(struct jdi_panel *jdi)
{
	struct mipi_dsi_device *dsi = jdi->dsi;
	int ret;

	dsi->mode_flags |= MIPI_DSI_MODE_LPM;

	ret = mipi_dsi_dcs_set_display_on(dsi);
	if (ret < 0)
		return ret;

	return 0;
}

static int jdi_panel_off(struct jdi_panel *jdi)
{
	struct mipi_dsi_device *dsi = jdi->dsi;
	int ret;

	dsi->mode_flags &= ~MIPI_DSI_MODE_LPM;

	ret = mipi_dsi_dcs_set_display_off(dsi);
	if (ret < 0)
		return ret;

	ret = mipi_dsi_dcs_enter_sleep_mode(dsi);
	if (ret < 0)
		return ret;

	ret = mipi_dsi_dcs_set_tear_off(dsi);
	if (ret < 0)
		return ret;

	msleep(100);

	return 0;
}

static int jdi_panel_disable(struct drm_panel *panel)
{
	struct jdi_panel *jdi = to_jdi_panel(panel);

	if (!jdi->enabled)
		return 0;

	DRM_DEBUG("disable\n");

	if (jdi->backlight) {
		jdi->backlight->props.power = FB_BLANK_POWERDOWN;
		backlight_update_status(jdi->backlight);
	}

	jdi->enabled = false;

	return 0;
}

static int jdi_panel_unprepare(struct drm_panel *panel)
{
	struct jdi_panel *jdi = to_jdi_panel(panel);
	struct regulator_bulk_data *s = jdi->supplies;
	int num = PANEL_DEV_REGULATOR_MAX;
	struct device *dev = &jdi->dsi->dev;
	int ret;

	if (!jdi->prepared)
		return 0;

	DRM_DEBUG("unprepare\n");

	ret = jdi_panel_off(jdi);
	if (ret) {
		dev_err(panel->dev, "failed to set panel off: %d\n", ret);
		return ret;
	}

	ret = regulator_bulk_disable(num, s);
	if (ret < 0) {
		dev_err(dev, "regulator disable failed, %d\n", ret);
		return ret;
	}

	if (jdi->vcc_gpio)
		gpiod_set_value(jdi->vcc_gpio, 0);

	if (jdi->reset_gpio)
		gpiod_set_value(jdi->reset_gpio, 0);

	if (jdi->enable_gpio)
		gpiod_set_value(jdi->enable_gpio, 0);

	jdi->prepared = false;

	return 0;
}

static int jdi_panel_prepare(struct drm_panel *panel)
{
	struct jdi_panel *jdi = to_jdi_panel(panel);
	struct regulator_bulk_data *s = jdi->supplies;
	int num = PANEL_DEV_REGULATOR_MAX;
	struct device *dev = &jdi->dsi->dev;
	int ret;

	if (jdi->prepared)
		return 0;

	DRM_DEBUG("prepare\n");

	ret = regulator_bulk_enable(num, s);
	if (ret < 0) {
		dev_err(dev, "regulator enable failed, %d\n", ret);
		return ret;
	}

	msleep(20);

	if (jdi->vcc_gpio) {
		gpiod_set_value(jdi->vcc_gpio, 1);
		usleep_range(10, 20);
	}

	if (jdi->reset_gpio) {
		gpiod_set_value(jdi->reset_gpio, 1);
		usleep_range(10, 20);
	}

	if (jdi->pwm_gpio) {
		gpiod_set_value(jdi->pwm_gpio, 1);
		usleep_range(10, 20);
	}

	if (jdi->enable_gpio) {
		gpiod_set_value(jdi->enable_gpio, 1);
		usleep_range(10, 20);
	}

	ret = jdi_panel_init(jdi);
	if (ret) {
		dev_err(panel->dev, "failed to init panel: %d\n", ret);
		goto poweroff;
	}

	ret = jdi_panel_on(jdi);
	if (ret) {
		dev_err(panel->dev, "failed to set panel on: %d\n", ret);
		goto poweroff;
	}

	jdi->prepared = true;

	return 0;

poweroff:
	if (jdi->reset_gpio)
		gpiod_set_value(jdi->reset_gpio, 0);
	if (jdi->enable_gpio)
		gpiod_set_value(jdi->enable_gpio, 0);
	if (jdi->vcc_gpio)
		gpiod_set_value(jdi->vcc_gpio, 0);

	return ret;
}

static int jdi_panel_enable(struct drm_panel *panel)
{
	struct jdi_panel *jdi = to_jdi_panel(panel);

	if (jdi->enabled)
		return 0;

	DRM_DEBUG("enable\n");

	if (jdi->backlight) {
		jdi->backlight->props.power = FB_BLANK_UNBLANK;
		backlight_update_status(jdi->backlight);
	}

	jdi->enabled = true;

	return 0;
}

static const struct drm_display_mode default_mode = {
		.clock = 155493,
		.hdisplay = 1200,
		.hsync_start = 1200 + 48,
		.hsync_end = 1200 + 48 + 32,
		.htotal = 1200 + 48 + 32 + 60,
		.vdisplay = 1920,
		.vsync_start = 1920 + 3,
		.vsync_end = 1920 + 3 + 5,
		.vtotal = 1920 + 3 + 5 + 6,
		.vrefresh = 60,
		.flags = 0,
};

static int jdi_panel_get_modes(struct drm_panel *panel)
{
	struct drm_display_mode *mode;
	struct jdi_panel *jdi = to_jdi_panel(panel);
	struct device *dev = &jdi->dsi->dev;

	mode = drm_mode_duplicate(panel->drm, &default_mode);
	if (!mode) {
		dev_err(dev, "failed to add mode %ux%ux@%u\n",
			default_mode.hdisplay, default_mode.vdisplay,
			default_mode.vrefresh);
		return -ENOMEM;
	}

	drm_mode_set_name(mode);

	drm_mode_probed_add(panel->connector, mode);

	return 1;
}

static const struct drm_panel_funcs jdi_panel_funcs = {
	.disable = jdi_panel_disable,
	.unprepare = jdi_panel_unprepare,
	.prepare = jdi_panel_prepare,
	.enable = jdi_panel_enable,
	.get_modes = jdi_panel_get_modes,
};

static const struct of_device_id jdi_of_match[] = {
	{ .compatible = "jdi,lt070me05000", },
	{ }
};
MODULE_DEVICE_TABLE(of, jdi_of_match);

static int jdi_panel_add(struct jdi_panel *jdi)
{
	struct device *dev = &jdi->dsi->dev;
	struct regulator_bulk_data *s = jdi->supplies;
	int num = PANEL_DEV_REGULATOR_MAX;
	int i, ret;

	jdi->mode = &default_mode;

	for (i = 0; i < num; i++)
		s[i].supply = regs[i];

	ret = devm_regulator_bulk_get(dev, num, s);
	if (ret < 0) {
		dev_err(dev, "%s: failed to init regulator, ret=%d\n",
			__func__, ret);
		return ret;
	}

	jdi->vcc_gpio = devm_gpiod_get(dev, "vcc", GPIOD_OUT_LOW);
	if (IS_ERR(jdi->vcc_gpio)) {
		dev_err(dev, "cannot get vcc-gpio %ld\n",
			PTR_ERR(jdi->vcc_gpio));
		jdi->vcc_gpio = NULL;
	} else {
		gpiod_direction_output(jdi->vcc_gpio, 0);
	}

	jdi->reset_gpio = devm_gpiod_get(dev, "reset", GPIOD_OUT_LOW);
	if (IS_ERR(jdi->reset_gpio)) {
		dev_err(dev, "cannot get reset-gpios %ld\n",
			PTR_ERR(jdi->reset_gpio));
		jdi->reset_gpio = NULL;
	} else {
		gpiod_direction_output(jdi->reset_gpio, 0);
	}

	jdi->enable_gpio = devm_gpiod_get(dev, "enable", GPIOD_OUT_LOW);
	if (IS_ERR(jdi->enable_gpio)) {
		dev_err(dev, "cannot get enable-gpio %ld\n",
			PTR_ERR(jdi->enable_gpio));
		jdi->enable_gpio = NULL;
	} else {
		gpiod_direction_output(jdi->enable_gpio, 0);
	}

	jdi->pwm_gpio = devm_gpiod_get(dev, "pwm", GPIOD_OUT_LOW);
	if (IS_ERR(jdi->pwm_gpio)) {
		dev_err(dev, "cannot get pwm-gpio %ld\n",
			PTR_ERR(jdi->pwm_gpio));
		jdi->pwm_gpio = NULL;
	} else {
		gpiod_direction_output(jdi->pwm_gpio, 0);
	}

	jdi->backlight = drm_panel_create_dsi_backlight(jdi->dsi);

	drm_panel_init(&jdi->base);
	jdi->base.funcs = &jdi_panel_funcs;
	jdi->base.dev = &jdi->dsi->dev;

	ret = drm_panel_add(&jdi->base);
	if (ret < 0)
		return ret;

	return 0;
}

static void jdi_panel_del(struct jdi_panel *jdi)
{
	if (jdi->base.dev)
		drm_panel_remove(&jdi->base);
}

static int jdi_panel_probe(struct mipi_dsi_device *dsi)
{
	struct jdi_panel *jdi;
	int ret;

	dsi->lanes = 4;
	dsi->format = MIPI_DSI_FMT_RGB888;
	dsi->mode_flags =  MIPI_DSI_MODE_VIDEO_HSE | MIPI_DSI_MODE_VIDEO |
			MIPI_DSI_MODE_VIDEO_HFP | MIPI_DSI_MODE_VIDEO_HBP |
			MIPI_DSI_MODE_VIDEO_HSA |
			MIPI_DSI_CLOCK_NON_CONTINUOUS;

	jdi = devm_kzalloc(&dsi->dev, sizeof(*jdi), GFP_KERNEL);
	if (!jdi)
		return -ENOMEM;

	mipi_dsi_set_drvdata(dsi, jdi);

	jdi->dsi = dsi;

	ret = jdi_panel_add(jdi);
	if (ret < 0)
		return ret;

	return mipi_dsi_attach(dsi);
}

static int jdi_panel_remove(struct mipi_dsi_device *dsi)
{
	struct jdi_panel *jdi = mipi_dsi_get_drvdata(dsi);
	int ret;

	ret = jdi_panel_disable(&jdi->base);
	if (ret < 0)
		dev_err(&dsi->dev, "failed to disable panel: %d\n", ret);

	ret = mipi_dsi_detach(dsi);
	if (ret < 0)
		dev_err(&dsi->dev, "failed to detach from DSI host: %d\n",
			ret);

	drm_panel_detach(&jdi->base);
	jdi_panel_del(jdi);

	return 0;
}

static void jdi_panel_shutdown(struct mipi_dsi_device *dsi)
{
	struct jdi_panel *jdi = mipi_dsi_get_drvdata(dsi);

	jdi_panel_disable(&jdi->base);
}

static struct mipi_dsi_driver jdi_panel_driver = {
	.driver = {
		.name = "panel-jdi-lt070me05000",
		.of_match_table = jdi_of_match,
	},
	.probe = jdi_panel_probe,
	.remove = jdi_panel_remove,
	.shutdown = jdi_panel_shutdown,
};
module_mipi_dsi_driver(jdi_panel_driver);

MODULE_AUTHOR("Sumit Semwal <sumit.semwal@linaro.org>");
MODULE_AUTHOR("Vinay Simha BN <simhavcs@gmail.com>");
MODULE_DESCRIPTION("JDI WUXGA LT070ME05000 DSI video mode panel driver");
MODULE_LICENSE("GPL v2");
