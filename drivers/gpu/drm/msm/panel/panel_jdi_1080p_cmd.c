/*
 * Copyright (C) 2014 InforceComputing 
 * Author: Vinay Simha BN <vinaysimha@inforcecomputing.com>
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
#include <linux/gpio.h>
#include <linux/regulator/msm-gpio-regulator.h>
#include <linux/mfd/pm8xxx/pm8921.h>
#include <linux/mfd/pm8xxx/pm8821.h>

#include "panel.h"

#define PM8921_GPIO_BASE                NR_GPIO_IRQS
#define PM8921_GPIO_PM_TO_SYS(pm_gpio)  (pm_gpio - 1 + PM8921_GPIO_BASE)
#define PM8921_MPP_BASE                 (PM8921_GPIO_BASE + PM8921_NR_GPIOS)
#define PM8921_MPP_PM_TO_SYS(pm_mpp)    (pm_mpp - 1 + PM8921_MPP_BASE)
#define PM8921_IRQ_BASE                 (NR_MSM_IRQS + NR_GPIO_IRQS)

#define PM8821_MPP_BASE                 (PM8921_MPP_BASE + PM8921_NR_MPPS)
#define PM8821_MPP_PM_TO_SYS(pm_mpp)    (pm_mpp - 1 + PM8821_MPP_BASE)

struct panel_jdi {
	struct panel base;
	struct mipi_adapter *mipi;
	struct regulator *reg_l11_avdd;
	struct regulator *reg_lvs7_vddio;
	struct regulator *reg_l17;
	struct regulator *reg_lvs5;
	struct regulator *reg_s4_iovdd;
	int pmic8921_23; /* panel VCC */
	int gpio_LCM_XRES_SR2; /* JDI reset pin */
	int gpio_LCD_BL_EN;
	int te_gpio;
};
#define to_panel_jdi(x) container_of(x, struct panel_jdi, base)

static char set_tear_off[2] = {0x34, 0x00};
static char set_tear_on[2] = {0x35, 0x00};

static char sw_reset[2] = {0x01, 0x00}; /* DTYPE_DCS_WRITE */
static char enter_sleep[2] = {0x10, 0x00}; /* DTYPE_DCS_WRITE */
static char exit_sleep[2] = {0x11, 0x00}; /* DTYPE_DCS_WRITE */
static char display_off[2] = {0x28, 0x00}; /* DTYPE_DCS_WRITE */
static char display_on[2] = {0x29, 0x00}; /* DTYPE_DCS_WRITE */

static char MCAP[2] = {0xB0, 0x00};
static char MCAP2[2] = {0xB0, 0x03};
static char interface_setting[2] = {0xb3, 0x6f};
/*TODO : need to check the registers for interface setting for this panel*/
//static char interface_setting[6] = {0xB3, 0x04, 0x08, 0x00, 0x22, 0x00};
static char interface_ID_setting[2] = {0xB4, 0x0C};
static char tear_scan_line[3] = {0x44, 0x03, 0x00};
static char DSI_control[3] = {0xB6, 0x3A, 0xD3};
static char set_pixel_format[2] = {0x3A, 0x77};
static char set_column_addr[5] = {0x2A, 0x00, 0x00, 0x04, 0xAF};
static char set_page_addr[5] = {0x2B, 0x00, 0x00, 0x07, 0x7F};

/* for fps control, set fps to 60.32Hz */
static char LTPS_timing_setting[2] = {0xC6, 0x78};
static char sequencer_timing_control[2] = {0xD6, 0x01};

/* set brightness */
static char write_display_brightness[] = {0x51, 0xff};
/* enable LEDPWM pin output, turn on LEDPWM output, turn off pwm dimming */
static char write_control_display[] = {0x53, 0x24};
/* choose cabc mode, 0x00(-0%), 0x01(-15%), 0x02(-40%), 0x03(-54%),
    disable SRE(sunlight readability enhancement) */
static char write_cabc[] = {0x55, 0x00};
/* for cabc mode 0x1(-15%) */
static char backlight_control1[] = {0xB8, 0x07, 0x87, 0x26, 0x18, 0x00, 0x32};
/* for cabc mode 0x2(-40%) */
static char backlight_control2[] = {0xB9, 0x07, 0x75, 0x61, 0x20, 0x16, 0x87};
/* for cabc mode 0x3(-54%) */
static char backlight_control3[] = {0xBA, 0x07, 0x70, 0x81, 0x20, 0x45, 0xB4};
/* for pwm frequency and dimming control */
static char backlight_control4[] = {0xCE, 0x7D, 0x40, 0x48, 0x56, 0x67, 0x78,
                0x88, 0x98, 0xA7, 0xB5, 0xC3, 0xD1, 0xDE, 0xE9, 0xF2, 0xFA,
                0xFF, 0x37, 0xF5, 0x0F, 0x0F, 0x42, 0x00};

static void panel_jdi_destroy(struct panel *panel)
{
	struct panel_jdi *panel_jdi = to_panel_jdi(panel);
	kfree(panel_jdi);
}

static int panel_jdi_power_on(struct panel *panel)
{
	struct drm_device *dev = panel->dev;
	struct panel_jdi *panel_jdi = to_panel_jdi(panel);
	int ret = 0;

	ret = regulator_set_optimum_mode(panel_jdi->reg_s4_iovdd, 100000);
	if (ret < 0) {
		dev_err(dev->dev, "failed to set s4 mode: %d\n", ret);
		goto fail1;
	}
	ret = regulator_set_optimum_mode(panel_jdi->reg_l11_avdd, 110000);
	if (ret < 0) {
		dev_err(dev->dev, "failed to set l11 mode: %d\n", ret);
		goto fail1;
	}

	ret = regulator_enable(panel_jdi->reg_s4_iovdd);
	if (ret) {
		dev_err(dev->dev, "failed to enable s4: %d\n", ret);
		goto fail1;
	}
	
	ret = regulator_enable(panel_jdi->reg_l11_avdd);
	if (ret) {
		dev_err(dev->dev, "failed to enable l11: %d\n", ret);
		goto fail1;
	}

	ret = regulator_enable(panel_jdi->reg_l17);
        if (ret) {
                dev_err(dev->dev, "failed to enable l17: %d\n", ret);
                goto fail1;
        }

	udelay(100);

	ret = regulator_enable(panel_jdi->reg_lvs7_vddio);
	if (ret) {
		dev_err(dev->dev, "failed to enable lvs7: %d\n", ret);
		goto fail2;
	}
	ret = regulator_enable(panel_jdi->reg_lvs5);
	if (ret) {
		dev_err(dev->dev, "failed to enable lvs5: %d\n", ret);
		goto fail2;
	}
//	mdelay(2);
	msleep_interruptible(8);
	gpio_set_value_cansleep(panel_jdi->pmic8921_23, 1);
	msleep(20);

	gpio_set_value_cansleep(panel_jdi->gpio_LCD_BL_EN, 1);

	gpio_set_value_cansleep(panel_jdi->gpio_LCM_XRES_SR2, 1);
        mdelay(1);/*Reset display 1 ms*/
        gpio_set_value_cansleep(panel_jdi->gpio_LCM_XRES_SR2, 0);
        usleep(50);
        gpio_set_value_cansleep(panel_jdi->gpio_LCM_XRES_SR2, 1);
        mdelay(5);

	return 0;

fail2:
	regulator_disable(panel_jdi->reg_lvs7_vddio);
fail1:
	regulator_disable(panel_jdi->reg_l11_avdd);
	
	return ret;
}

static int panel_jdi_power_off(struct panel *panel)
{
	struct drm_device *dev = panel->dev;
	struct panel_jdi *panel_jdi = to_panel_jdi(panel);
	int ret;

	gpio_set_value_cansleep(panel_jdi->gpio_LCD_BL_EN, 0);
	gpio_set_value_cansleep(panel_jdi->pmic8921_23, 0);
	udelay(100);
	gpio_set_value_cansleep(panel_jdi->gpio_LCM_XRES_SR2, 0);
	udelay(100);

	ret = regulator_disable(panel_jdi->reg_s4_iovdd);
	if (ret)
		dev_err(dev->dev, "failed to disable s4: %d\n", ret);

	udelay(100);
	ret = regulator_disable(panel_jdi->reg_l11_avdd);
	if (ret)
		dev_err(dev->dev, "failed to disable l8: %d\n", ret);

	udelay(100);

	ret = regulator_disable(panel_jdi->reg_lvs7_vddio);
	if (ret)
		dev_err(dev->dev, "failed to disable lvs7: %d\n", ret);

	ret = regulator_disable(panel_jdi->reg_l17);
	if (ret)
		dev_err(dev->dev, "failed to disable l17: %d\n", ret);

	ret = regulator_disable(panel_jdi->reg_lvs5);
	if (ret)
		dev_err(dev->dev, "failed to disable lvs5: %d\n", ret);

	return 0;
}

static int panel_jdi_on(struct panel *panel)
{
	struct panel_jdi *panel_jdi = to_panel_jdi(panel);
	struct mipi_adapter *mipi = panel_jdi->mipi;
	int ret = 0;

	DRM_DEBUG_KMS("panel on\n");

	ret = panel_jdi_power_on(panel);
	if (ret)
		return ret;

	mipi_set_panel_config(mipi, &(struct mipi_panel_config){
		.cmd_mode = false,
		.traffic_mode = NON_BURST_SYNCH_EVENT,
                .bllp_power_stop = true,
                .eof_bllp_power_stop = true,
                .hsa_power_stop = false,
                .hbp_power_stop = true,
                .hfp_power_stop = true,
                .pulse_mode_hsa_he = true,
		.interleave_max = false,
		.rgb_swap = SWAP_RGB,
		.format = CMD_DST_FORMAT_RGB888,
		.insert_dcs_cmd = true,
		.wr_mem_continue = 0x3c,
		.wr_mem_start = 0x2c,
		.dma_trigger = TRIGGER_SW,
		.mdp_trigger = TRIGGER_NONE,
		.te = false,
		.dlane_swap = 0,
		.t_clk_pre = 0x1c,
		.t_clk_post = 0x04,
		.rx_eot_ignore = false,
		.tx_eot_append = true,
		.ecc_check = false,
		.crc_check = false,
		.phy = {
			/* regulator */
			{0x03, 0x0a, 0x04, 0x00, 0x20},
		        /* timing   */
		        {0x66, 0x26, 0x38, 0x00, 0x3e, 0xe6, 0x1e, 0x9b,
			0x3e, 0x03, 0x04, 0xa0},
			/* phy ctrl */
		        {0x5f, 0x00, 0x00, 0x10},
		        /* strength */
		        {0xff, 0x00, 0x06, 0x00},
		        /* pll control */
			{0x00, /*common 8064*/
                         0xf3, 0x31, 0xda, /* panel specific */
                         0x00, 0x10, 0x0f, 0x62, /*panel specific */
                         0x70, 0x07, 0x01,
                         0x00, 0x14, 0x03, 0x00, 0x02, /*common 8064*/
                         0x0e, 0x01, 0x00, 0x01}, /* common 8064*/
		},
	});

	mipi_set_bus_config(mipi, &(struct mipi_bus_config){
		.low_power = false,
		.lanes = 0xf,
	});

	mipi_on(mipi);

	// if (lcd_pwm == LCD_PWM_TYPE_B) { /* set pwm frequency to 22K */
	backlight_control4[18] = 0x04;
	backlight_control4[19] = 0x00;

	mipi->wait=10;
	mipi_dcs_swrite(mipi, true, 0, false, sw_reset[0]);
	mipi->wait=0;
	mipi_dcs_swrite1(mipi, true, 0, false, set_pixel_format);
	mipi_dcs_lwrite(mipi, true, 0, set_column_addr);
	mipi->wait=120;
	mipi_dcs_lwrite(mipi, true, 0, set_page_addr);
	mipi->wait=5;
	mipi_dcs_swrite1(mipi, true, 0, false, set_tear_on);
	mipi->wait=0;
	mipi_lwrite(mipi, true, 0, tear_scan_line);
	mipi_dcs_swrite1(mipi, true, 0, false, write_display_brightness);
	mipi_dcs_swrite1(mipi, true, 0, false, write_control_display);
	mipi_dcs_swrite1(mipi, true, 0, false, write_cabc);
	mipi->wait=120;
        mipi_dcs_swrite(mipi, true, 0, false, exit_sleep[0]);
	mipi->wait=10;
	mipi_gen_write(mipi, true, 0, MCAP);
	mipi->wait=10;
	mipi_lwrite(mipi, true, 0, interface_setting);
	mipi->wait=20;
	mipi_lwrite(mipi, true, 0, backlight_control4);
	mipi->wait=0;
	mipi_gen_write(mipi, true, 0, MCAP2);
	mipi->wait=16;
        mipi_dcs_swrite(mipi, true, 0, false, display_on[0]);
        mdelay(150);
if(0){
	mipi_lwrite(mipi, true, 0, interface_ID_setting);
	mipi_lwrite(mipi, true, 0, DSI_control);
	mipi_gen_write(mipi, true, 0, LTPS_timing_setting);
	mipi_dcs_swrite1(mipi, true, 0, false, sequencer_timing_control);
	mipi_lwrite(mipi, true, 0, backlight_control1);
	mipi_lwrite(mipi, true, 0, backlight_control2);
	mipi_lwrite(mipi, true, 0, backlight_control3);
}

	return 0;
}

static int panel_jdi_off(struct panel *panel)
{
	struct panel_jdi *panel_jdi = to_panel_jdi(panel);
	struct mipi_adapter *mipi = panel_jdi->mipi;
	int ret;

	DRM_DEBUG_KMS("panel off\n");

	mipi_set_bus_config(mipi, &(struct mipi_bus_config){
		.low_power = true,
		.lanes = 0xf,
	});

	mipi->wait=20;
	mipi_dcs_swrite(mipi, true, 0, false, display_off[0]);
	mdelay(5);
	mipi->wait=80;
	mipi_dcs_swrite(mipi, true, 0, false, enter_sleep[0]);
	mdelay(5);
	mipi->wait=0;
	mipi_dcs_swrite1(mipi, true, 0, false, set_tear_off);
	mdelay(5);

	mipi_off(mipi);

	ret = panel_jdi_power_off(panel);
	if (ret)
		return ret;

	return 0;
}

static struct drm_display_mode *panel_jdi_mode(struct panel *panel)
{
	struct drm_display_mode *mode = drm_mode_create(panel->dev);
	u32 hbp, hfp, vbp, vfp, hspw, vspw;

	snprintf(mode->name, sizeof(mode->name), "1200x1920");

	mode->clock = 155000;

	hbp = 60;
	hfp = 48;
	vbp = 6;
	vfp = 3;
	hspw = 32;
	vspw = 5;

	mode->hdisplay = 1200;
	mode->hsync_start = mode->hdisplay + hfp;
	mode->hsync_end = mode->hsync_start + hspw;
	mode->htotal = mode->hsync_end + hbp;

	mode->vdisplay = 1920;
	mode->vsync_start = mode->vdisplay + vfp;
	mode->vsync_end = mode->vsync_start + vspw;
	mode->vtotal = mode->vsync_end + vbp;

	mode->flags = MIPI_DSI_MODE_VIDEO;

	return mode;
}

static const struct panel_funcs panel_jdi_funcs = {
		.destroy = panel_jdi_destroy,
		.on = panel_jdi_on,
		.off = panel_jdi_off,
		.mode = panel_jdi_mode,
};

struct panel *panel_jdi_1080p_init(struct drm_device *dev,
		// XXX uggg.. maybe we should just pass in a config structure
		// pre-populated with regulators, gpio's, etc??  the panel
		// needs the drm device, but we need the pdev to lookup the
		// regulators, etc, currently.. and for msm the pdev is different
		// device from the drm device..
		struct platform_device *pdev,
		struct mipi_adapter *mipi)
{
	struct panel_jdi *panel_jdi;
	struct panel *panel = NULL;
	int ret;

	panel_jdi = kzalloc(sizeof(*panel_jdi), GFP_KERNEL);
	if (!panel_jdi) {
		ret = -ENOMEM;
		goto fail;
	}

	panel_jdi->mipi = mipi;

	panel = &panel_jdi->base;
	ret = panel_init(dev, panel, &panel_jdi_funcs);
	if (ret)
		goto fail;

	/* Maybe GPIO/regulator/etc stuff should come from DT or similar..
	 * but we can sort that out when there is some other device that
	 * uses the same panel.
	 */
	panel_jdi->gpio_LCD_BL_EN = PM8921_GPIO_PM_TO_SYS(36);
	ret = gpio_request(panel_jdi->gpio_LCD_BL_EN, "LCD_BL_EN");
	if (ret) {
		dev_err(dev->dev, "failed to request LCD_BL_EN : %d\n", ret);
		goto fail;
	}
	ret = gpio_export(panel_jdi->gpio_LCD_BL_EN, true);
	if (ret) {
		dev_err(dev->dev, "failed to request gpio export LCD_BL_EN: %d\n", ret);
		goto fail;
	}
	ret = gpio_direction_output(panel_jdi->gpio_LCD_BL_EN, 0);
	if (ret) {
		dev_err(dev->dev, "failed to request gpio direction output LCD_BL_EN: %d\n", ret);
		goto fail;
	}

	panel_jdi->pmic8921_23 = PM8921_GPIO_PM_TO_SYS(23);
	ret = gpio_request(panel_jdi->pmic8921_23, "EN_VDD_BL");
	if (ret) {
		dev_err(dev->dev, "failed to request EN_VDD_BL : %d\n", ret);
		goto fail;
	}
	ret = gpio_export(panel_jdi->pmic8921_23, true);
	if (ret) {
		dev_err(dev->dev, "failed to request gpio export mpp: %d\n", ret);
		goto fail;
	}
	ret = gpio_direction_output(panel_jdi->pmic8921_23, 0);
	if (ret) {
		dev_err(dev->dev, "failed to request gpio direction output mpp: %d\n", ret);
		goto fail;
	}

 	panel_jdi->te_gpio = 0;
        ret = gpio_request(panel_jdi->te_gpio, "MDP_VSYNC");
        if (ret) {
                dev_err(dev->dev, "failed to request LCM_TE : %d\n", ret);
		goto fail;
        }
        ret = gpio_export(panel_jdi->te_gpio, true);
        if (ret) {
                dev_err(dev->dev, "failed to request gpio export lcm_te: %d\n", ret);
		goto fail;
        }
        ret = gpio_direction_input(panel_jdi->te_gpio);
        if (ret) {
                dev_err(dev->dev, "failed to request gpio direction input lcm_te: %d\n", ret);
		goto fail;
        }
		
	panel_jdi->gpio_LCM_XRES_SR2 = 54;
	ret = gpio_request(panel_jdi->gpio_LCM_XRES_SR2, "LCM_XRES");
	if (ret) {
		dev_err(dev->dev, "failed to request LCM_XRES : %d\n", ret);
		goto fail;
	}

	ret = gpio_export(panel_jdi->gpio_LCM_XRES_SR2, true);
	if (ret) {
		dev_err(dev->dev, "failed to request gpio export mpp: %d\n", ret);
		goto fail;
	}
//	ret = gpio_direction_output(panel_jdi->gpio_LCM_XRES_SR2, 0);
	ret = gpio_tlmm_config(
		GPIO_CFG(panel_jdi->gpio_LCM_XRES_SR2, 0,
			GPIO_CFG_OUTPUT, GPIO_CFG_NO_PULL, GPIO_CFG_8MA),
			GPIO_CFG_ENABLE);
	if (ret) {
		dev_err(dev->dev, "failed to request gpio direction output XRES: %d\n", ret);
		goto fail;
	}

	panel_jdi->reg_s4_iovdd = devm_regulator_get(dev->dev, "dsi1_s4_iovdd");
	if (IS_ERR(panel_jdi->reg_s4_iovdd)) {
		ret = PTR_ERR(panel_jdi->reg_s4_iovdd);
		dev_err(dev->dev, "failed to request dsi_iovdd regulator: %d\n", ret);
		goto fail;
	}
	panel_jdi->reg_l11_avdd = devm_regulator_get(dev->dev, "dsi1_avdd");
	if (IS_ERR(panel_jdi->reg_l11_avdd)) {
		ret = PTR_ERR(panel_jdi->reg_l11_avdd);
		dev_err(dev->dev, "failed to request dsi_avdd regulator: %d\n", ret);
		goto fail;
	}

	panel_jdi->reg_lvs7_vddio = devm_regulator_get(dev->dev, "dsi1_vddio");
	if (IS_ERR(panel_jdi->reg_lvs7_vddio)) {
		ret = PTR_ERR(panel_jdi->reg_lvs7_vddio);
		dev_err(dev->dev, "failed to request dsi1__vddio regulator: %d\n", ret);
		goto fail;
	}

	panel_jdi->reg_l17 = devm_regulator_get(dev->dev, "pwm_power");
	if (IS_ERR(panel_jdi->reg_l17)) {
		ret = PTR_ERR(panel_jdi->reg_l17);
		dev_err(dev->dev, "failed to request pwm_power regulator: %d\n", ret);
		goto fail;
	}
	
	panel_jdi->reg_lvs5 = devm_regulator_get(dev->dev, "JDI_IOVCC");
	if (IS_ERR(panel_jdi->reg_lvs5)) {
		ret = PTR_ERR(panel_jdi->reg_lvs5);
		dev_err(dev->dev, "failed to request lvs5 regulator: %d\n", ret);
		goto fail;
	}

	ret = regulator_set_voltage(panel_jdi->reg_s4_iovdd,  1800000, 1800000);
	if (ret) {
		dev_err(dev->dev, "set_voltage s4 failed: %d\n", ret);
		goto fail;
	}

	ret = regulator_set_voltage(panel_jdi->reg_l11_avdd,  3000000, 3000000);
	if (ret) {
		dev_err(dev->dev, "set_voltage l8 failed: %d\n", ret);
		goto fail;
	}
	
	ret = regulator_set_voltage(panel_jdi->reg_l17,  3000000, 3000000);
	if (ret) {
		dev_err(dev->dev, "set_voltage l8 failed: %d\n", ret);
		goto fail;
	}

	return panel;
fail:
	if (panel)
		panel_jdi_destroy(panel);
	return ERR_PTR(ret);
}
