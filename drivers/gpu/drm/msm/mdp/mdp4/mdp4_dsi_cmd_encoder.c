/*
 * Copyright (C) 2017
 * Author: Vinay Simha BN <simhavcs@gmail.com>
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

#include "mdp4_kms.h"

#include "drm_crtc.h"
#include "drm_crtc_helper.h"

struct mdp4_dsi_encoder {
	struct drm_encoder base;
	struct drm_panel *panel;
	bool enabled;
	u32 bsc;
};

#define to_mdp4_dsi_encoder(x) container_of(x, struct mdp4_dsi_encoder, base)

static struct mdp4_kms *get_kms(struct drm_encoder *encoder)
{
	struct msm_drm_private *priv = encoder->dev->dev_private;

	return to_mdp4_kms(to_mdp_kms(priv->kms));
}

#ifdef CONFIG_MSM_BUS_SCALING
#include <mach/board.h>
/* not ironically named at all.. no, really.. */
static void bs_init(struct mdp4_dsi_encoder *mdp4_dsi_encoder)
{
	struct drm_device *dev = mdp4_dsi_encoder->base.dev;
	struct msm_panel_common_pdata *dsi_pdata = mdp4_find_pdata("mdp.0");

	if (!dsi_pdata) {
		dev_err(dev->dev, "could not find mipi_dsi pdata\n");
		return;
	}

	if (dsi_pdata->mdp_bus_scale_table) {
		mdp4_dsi_encoder->bsc = msm_bus_scale_register_client(
				dsi_pdata->mdp_bus_scale_table);
		DBG("bus scale client: %08x", mdp4_dsi_encoder->bsc);
	}
}

static void bs_fini(struct mdp4_dsi_encoder *mdp4_dsi_encoder)
{
	if (mdp4_dsi_encoder->bsc) {
		msm_bus_scale_unregister_client(mdp4_dsi_encoder->bsc);
		mdp4_dsi_encoder->bsc = 0;
	}
}

static void bs_set(struct mdp4_dsi_encoder *mdp4_dsi_encoder, int idx)
{
	if (mdp4_dsi_encoder->bsc) {
		DBG("set bus scaling: %d", idx);
		msm_bus_scale_client_update_request(mdp4_dsi_encoder->bsc, idx);
	}
}
#else
static void bs_init(struct mdp4_dsi_encoder *mdp4_dsi_encoder) {}
static void bs_fini(struct mdp4_dsi_encoder *mdp4_dsi_encoder) {}
static void bs_set(struct mdp4_dsi_encoder *mdp4_dsi_encoder, int idx) {}
#endif

#define VSYNC_CLK_RATE 27000000
static int pingpong_tearcheck_setup(struct drm_encoder *encoder,
				    struct drm_display_mode *mode)
{
	struct mdp4_kms *mdp4_kms = get_kms(encoder);
	struct device *dev = encoder->dev->dev;
	u32 total_lines_x100, vclks_line, cfg;
	long vsync_clk_speed;

	DBG(" DSI : %s", __func__);

	if (IS_ERR_OR_NULL(mdp4_kms->vsync_clk)) {
		dev_err(dev, "vsync_clk is not initialized\n");
		return -EINVAL;
	}

	total_lines_x100 = mode->vtotal * mode->vrefresh;
	if (!total_lines_x100) {
		dev_err(dev, "%s: vtotal(%d) or vrefresh(%d) is 0\n",
			__func__, mode->vtotal, mode->vrefresh);
		return -EINVAL;
	}

	vsync_clk_speed = clk_round_rate(mdp4_kms->vsync_clk, VSYNC_CLK_RATE);
	if (vsync_clk_speed <= 0) {
		dev_err(dev, "vsync_clk round rate failed %ld\n",
			vsync_clk_speed);
		return -EINVAL;
	}
	vclks_line = vsync_clk_speed * 100 / total_lines_x100;

	cfg = mode->vtotal - 1;
	cfg <<= REG_MDP4_SYNCFG_HGT_LOC;
	cfg = REG_MDP4_SYNCFG_VSYNC_COUNTER_EN
		| REG_MDP4_SYNCFG_VSYNC_INT_EN;
	cfg |= MDP4_SYNC_CONFIG_VSYNC_COUNT(vclks_line);

	mdp4_write(mdp4_kms, REG_MDP4_SYNC_CFG_0, cfg);
	mdp4_write(mdp4_kms, REG_MDP4_PRIM_VSYNC_OUT_CTRL, BIT(0));
	mdp4_write(mdp4_kms, REG_MDP4_SEC_VSYNC_OUT_CTRL, BIT(0));
	mdp4_write(mdp4_kms, REG_MDP4_VSYNC_SEL, 0x20);
	mdp4_write(mdp4_kms, REG_MDP4_PRIM_VSYNC_INIT_VAL, mode->vdisplay);
	mdp4_write(mdp4_kms, REG_MDP4_PRIMARY_RD_PTR_IRQ, mode->vdisplay + 1);
	mdp4_write(mdp4_kms, REG_MDP4_PRIM_START_POS, mode->vdisplay);
	mdp4_write(mdp4_kms, REG_MDP4_SYNC_THRESH_P,
		   MDP4_SYNC_THRESH_START(4) |
		   MDP4_SYNC_THRESH_CONTINUE(4));

	return 0;
}

static int pingpong_tearcheck_enable(struct drm_encoder *encoder)
{
	struct mdp4_kms *mdp4_kms = get_kms(encoder);
	int ret;

	DBG(" DSI : %s", __func__);

	ret = clk_set_rate(mdp4_kms->vsync_clk,
			   clk_round_rate(mdp4_kms->vsync_clk, VSYNC_CLK_RATE));
	if (ret) {
		dev_err(encoder->dev->dev,
			"vsync_clk clk_set_rate failed, %d\n", ret);
		return ret;
	}

	ret = clk_prepare_enable(mdp4_kms->vsync_clk);
	if (ret) {
		dev_err(encoder->dev->dev,
			"vsync_clk clk_prepare_enable failed, %d\n", ret);
		return ret;
	}

	mdp4_write(mdp4_kms, REG_MDP4_TEAR_CHECK_EN, 1);

	return 0;
}

static void pingpong_tearcheck_disable(struct drm_encoder *encoder)
{
	struct mdp4_kms *mdp4_kms = get_kms(encoder);

	DBG(" DSI : %s", __func__);
	mdp4_write(mdp4_kms, REG_MDP4_TEAR_CHECK_EN, 0);
	clk_disable_unprepare(mdp4_kms->vsync_clk);
}

static void mdp4_dsi_encoder_destroy(struct drm_encoder *encoder)
{
	struct mdp4_dsi_encoder
		*mdp4_dsi_encoder = to_mdp4_dsi_encoder(encoder);
	bs_fini(mdp4_dsi_encoder);
	drm_encoder_cleanup(encoder);
	kfree(mdp4_dsi_encoder);
}

static const struct drm_encoder_funcs mdp4_dsi_encoder_funcs = {
	.destroy = mdp4_dsi_encoder_destroy,
};

static bool mdp4_dsi_encoder_mode_fixup(struct drm_encoder *encoder,
					const struct drm_display_mode *mode,
					struct drm_display_mode *adjusted_mode)
{
	return true;
}

static void mdp4_dsi_encoder_mode_set(struct drm_encoder *encoder,
				      struct drm_display_mode *mode,
				      struct drm_display_mode *adjusted_mode)
{
	mode = adjusted_mode;

	DBG("set mode: %d:\"%s\" %d %d %d %d %d %d %d %d %d %d 0x%x 0x%x",
	    mode->base.id, mode->name,
	    mode->vrefresh, mode->clock,
	    mode->hdisplay, mode->hsync_start,
	    mode->hsync_end, mode->htotal,
	    mode->vdisplay, mode->vsync_start,
	    mode->vsync_end, mode->vtotal,
	    mode->type, mode->flags);

	pingpong_tearcheck_setup(encoder, mode);
}

static void mdp4_dsi_encoder_disable(struct drm_encoder *encoder)
{
	struct mdp4_dsi_encoder
		*mdp4_dsi_encoder = to_mdp4_dsi_encoder(encoder);
	struct mdp4_kms *mdp4_kms = get_kms(encoder);

	if (!mdp4_dsi_encoder->enabled)
		return;

	pingpong_tearcheck_disable(encoder);

	mdp4_write(mdp4_kms, REG_MDP4_DSI_ENABLE, 0);

	/*
	 * Wait for a vsync so we know the ENABLE=0 latched before
	 * the (connector) source of the vsync's gets disabled,
	 * otherwise we end up in a funny state if we re-enable
	 * before the disable latches, which results that some of
	 * the settings changes for the new modeset (like new
	 * scanout buffer) don't latch properly..
	 */
	mdp_irq_wait(&mdp4_kms->base, MDP4_IRQ_PRIMARY_VSYNC);

	bs_set(mdp4_dsi_encoder, 0);

	mdp4_dsi_encoder->enabled = false;
}

static void mdp4_dsi_encoder_enable(struct drm_encoder *encoder)
{
	struct mdp4_dsi_encoder
		*mdp4_dsi_encoder = to_mdp4_dsi_encoder(encoder);
	struct mdp4_kms *mdp4_kms = get_kms(encoder);

	if (mdp4_dsi_encoder->enabled)
		return;

	 mdp4_crtc_set_config(encoder->crtc,
			      MDP4_DMA_CONFIG_PACK_ALIGN_MSB |
			      MDP4_DMA_CONFIG_DEFLKR_EN |
			      MDP4_DMA_CONFIG_DITHER_EN |
			      MDP4_DMA_CONFIG_R_BPC(BPC8) |
			      MDP4_DMA_CONFIG_G_BPC(BPC8) |
			      MDP4_DMA_CONFIG_B_BPC(BPC8) |
			      MDP4_DMA_CONFIG_PACK(0x21));

	mdp4_crtc_set_intf(encoder->crtc, INTF_DSI_CMD, 0);

	if (pingpong_tearcheck_enable(encoder))
		return;

	bs_set(mdp4_dsi_encoder, 1);

	mdp4_write(mdp4_kms, REG_MDP4_DSI_ENABLE, 1);

	mdp4_dsi_encoder->enabled = true;
}

static const struct drm_encoder_helper_funcs mdp4_dsi_encoder_helper_funcs = {
	.mode_fixup = mdp4_dsi_encoder_mode_fixup,
	.mode_set = mdp4_dsi_encoder_mode_set,
	.disable = mdp4_dsi_encoder_disable,
	.enable = mdp4_dsi_encoder_enable,
};

/* initialize encoder */
struct drm_encoder *mdp4_dsi_cmd_encoder_init(struct drm_device *dev)
{
	struct drm_encoder *encoder = NULL;
	struct mdp4_dsi_encoder *mdp4_dsi_encoder;
	int ret;

	mdp4_dsi_encoder = kzalloc(sizeof(*mdp4_dsi_encoder), GFP_KERNEL);
	if (!mdp4_dsi_encoder) {
		ret = -ENOMEM;
		goto fail;
	}

	encoder = &mdp4_dsi_encoder->base;

	drm_encoder_init(dev, encoder, &mdp4_dsi_encoder_funcs,
			 DRM_MODE_ENCODER_DSI, NULL);

	drm_encoder_helper_add(encoder, &mdp4_dsi_encoder_helper_funcs);

	bs_init(mdp4_dsi_encoder);

	return encoder;

fail:
	if (encoder)
		mdp4_dsi_encoder_destroy(encoder);

	return ERR_PTR(ret);
}
