/*
 * Copyright (C) 2014
 * Author: Vinay Simha <vinaysimha@inforcecomputing.com>
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
#include <drm/drm_mipi_dsi.h>

struct mdp4_dsi_encoder {
	struct drm_encoder base;
	struct drm_panel *panel;
	bool enabled;
	uint32_t bsc;
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

static void mdp4_dsi_encoder_destroy(struct drm_encoder *encoder)
{
	struct mdp4_dsi_encoder *mdp4_dsi_encoder = to_mdp4_dsi_encoder(encoder);
	bs_fini(mdp4_dsi_encoder);
	drm_encoder_cleanup(encoder);
	kfree(mdp4_dsi_encoder);
}

static const struct drm_encoder_funcs mdp4_dsi_encoder_funcs = {
	.destroy = mdp4_dsi_encoder_destroy,
};

static void mdp4_dsi_encoder_dpms(struct drm_encoder *encoder, int mode)
{
	struct mdp4_dsi_encoder *mdp4_dsi_encoder = to_mdp4_dsi_encoder(encoder);
	struct mdp4_kms *mdp4_kms = get_kms(encoder);
	bool enabled = (mode == DRM_MODE_DPMS_ON);
	//int ret;
	uint32_t data;

	DBG("mode=%d", mode);

	if (enabled == mdp4_dsi_encoder->enabled)
		return;

	if (enabled) {

		bs_set(mdp4_dsi_encoder, 1);
		
		mdp4_write(mdp4_kms, REG_MDP4_DSI_ENABLE, 1);

	} else {
		mdp4_write(mdp4_kms, REG_MDP4_DSI_ENABLE, 0);
		
		data = mdp4_read(mdp4_kms, 0x20c);
		data &= ~(1<<0);
		mdp4_write(mdp4_kms, 0x20c, data); /* TEAR_CHECK_EN */

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
	}

	mdp4_dsi_encoder->enabled = enabled;
}

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
	//struct mdp4_dsi_encoder *mdp4_dsi_encoder = to_mdp4_dsi_encoder(encoder);
	struct mdp4_kms *mdp4_kms = get_kms(encoder);
	uint32_t dsi_hsync_skew, vsync_period, vsync_len, ctrl_pol;
	uint32_t display_v_start, display_v_end;
	uint32_t hsync_start_x, hsync_end_x;
	uint32_t vsync_cnt_cfg;
	uint32_t vsync_cnt_cfg_dem;
	uint32_t refx100 = 6032;
	uint32_t mdp_vsync_clk_speed_hz;
	int vsync_above_th = 4;
	int vsync_start_th = 1;
	uint32_t cfg;
	uint32_t data;

	mode = adjusted_mode;

	DBG("set mode: %d:\"%s\" %d %d %d %d %d %d %d %d %d %d 0x%x 0x%x",
			mode->base.id, mode->name,
			mode->vrefresh, mode->clock,
			mode->hdisplay, mode->hsync_start,
			mode->hsync_end, mode->htotal,
			mode->vdisplay, mode->vsync_start,
			mode->vsync_end, mode->vtotal,
			mode->type, mode->flags);

	if(mode->flags & MIPI_DSI_MODE_VIDEO){

		DBG(" DSI: VIDEO MODE %s", __func__);
		ctrl_pol = 0;
		if (mode->flags & DRM_MODE_FLAG_NHSYNC)
			ctrl_pol |= MDP4_DSI_CTRL_POLARITY_HSYNC_LOW;
		if (mode->flags & DRM_MODE_FLAG_NVSYNC)
			ctrl_pol |= MDP4_DSI_CTRL_POLARITY_VSYNC_LOW;
		/* probably need to get DATA_EN polarity from panel.. */

		dsi_hsync_skew = 0;  /* get this from panel? */

		hsync_start_x = (mode->htotal - mode->hsync_start);
		hsync_end_x = mode->htotal - (mode->hsync_start - mode->hdisplay) - 1;
	
		vsync_period = mode->vtotal * mode->htotal;
		vsync_len = (mode->vsync_end - mode->vsync_start) * mode->htotal;
		display_v_start = (mode->vtotal - mode->vsync_start) * mode->htotal + dsi_hsync_skew;
		display_v_end = vsync_period - ((mode->vsync_start - mode->vdisplay) * mode->htotal) + dsi_hsync_skew - 1;

		mdp4_write(mdp4_kms, REG_MDP4_DSI_HSYNC_CTRL,
			MDP4_DSI_HSYNC_CTRL_PULSEW(mode->hsync_end - mode->hsync_start) |
				MDP4_DSI_HSYNC_CTRL_PERIOD(mode->htotal));
		mdp4_write(mdp4_kms, REG_MDP4_DSI_VSYNC_PERIOD, vsync_period);
		mdp4_write(mdp4_kms, REG_MDP4_DSI_VSYNC_LEN, vsync_len);
		mdp4_write(mdp4_kms, REG_MDP4_DSI_DISPLAY_HCTRL,
				MDP4_DSI_DISPLAY_HCTRL_START(hsync_start_x) |
				MDP4_DSI_DISPLAY_HCTRL_END(hsync_end_x));
		mdp4_write(mdp4_kms, REG_MDP4_DSI_DISPLAY_VSTART, display_v_start);
		mdp4_write(mdp4_kms, REG_MDP4_DSI_DISPLAY_VEND, display_v_end);

		mdp4_write(mdp4_kms, REG_MDP4_DSI_CTRL_POLARITY, ctrl_pol);
		mdp4_write(mdp4_kms, REG_MDP4_DSI_UNDERFLOW_CLR,
				MDP4_DSI_UNDERFLOW_CLR_ENABLE_RECOVERY |
				MDP4_DSI_UNDERFLOW_CLR_COLOR(0xff));
		mdp4_write(mdp4_kms, REG_MDP4_DSI_ACTIVE_HCTL,
				MDP4_DSI_ACTIVE_HCTL_START(0) |
				MDP4_DSI_ACTIVE_HCTL_END(0));
		mdp4_write(mdp4_kms, REG_MDP4_DSI_HSYNC_SKEW, dsi_hsync_skew);
		mdp4_write(mdp4_kms, REG_MDP4_DSI_BORDER_CLR, 0);
		mdp4_write(mdp4_kms, REG_MDP4_DSI_ACTIVE_VSTART, 0);
		mdp4_write(mdp4_kms, REG_MDP4_DSI_ACTIVE_VEND, 0);

	} else {
		DBG(" DSI: COMMAND MODE %s", __func__);

		if (IS_ERR_OR_NULL(mdp4_kms->vsync_clk)) {
        	        dev_err(encoder->dev->dev, "vsync_clk is not initialized\n");
	        }

		mdp_vsync_clk_speed_hz = clk_get_rate(mdp4_kms->vsync_clk);
	        if (mdp_vsync_clk_speed_hz <= 0) {
        	        dev_err(encoder->dev->dev, "vsync_clk round rate failed %zu\n",
								mdp_vsync_clk_speed_hz);
	        }

		vsync_cnt_cfg_dem = (refx100 * mode->vtotal) / 100;
		vsync_cnt_cfg = (mdp_vsync_clk_speed_hz) / vsync_cnt_cfg_dem;

		cfg = mode->vtotal - 1;
		cfg <<= REG_MDP4_SYNCFG_HGT_LOC;
		cfg |= REG_MDP4_SYNCFG_VSYNC_EXT_EN;
		cfg |= (REG_MDP4_SYNCFG_VSYNC_INT_EN | vsync_cnt_cfg);

		mdp4_write(mdp4_kms, REG_MDP4_SYNC_CFG_0, cfg);

		 /* line counter init value at the next pulse */
		mdp4_write(mdp4_kms, REG_MDP4_PRIM_VSYNC_INIT_VAL, mode->vdisplay);
		/*
		* external vsync source pulse width and
		* polarity flip
		*/
		mdp4_write(mdp4_kms, REG_MDP4_PRIM_VSYNC_OUT_CTRL, BIT(0));
		/* threshold */
		mdp4_write(mdp4_kms, 0x200,  (vsync_above_th << 16) |
	         (vsync_start_th));

		/* TE enabled */
		mdp4_write(mdp4_kms, 0x210, mode->vtotal - 1);
		data = mdp4_read(mdp4_kms, 0x20c);
		data |= (1 << 0);
		mdp4_write(mdp4_kms, 0x20c, data); /* TEAR_CHECK_EN */

		mdp4_write(mdp4_kms, 0x021c, 10); /* read pointer */

		/*
		 * configure dsi stream id
		 * dma_p = 0, dma_s = 1
		 */
		mdp4_write(mdp4_kms, 0x000a0, 0x10); /* read pointer */
		/* disable dsi trigger */
		mdp4_write(mdp4_kms, 0x000a4, 0x00);
	}
}

static void mdp4_dsi_encoder_prepare(struct drm_encoder *encoder)
{
	mdp4_dsi_encoder_dpms(encoder, DRM_MODE_DPMS_OFF);
}

static void mdp4_dsi_encoder_commit(struct drm_encoder *encoder)
{
	 mdp4_crtc_set_config(encoder->crtc,
                        MDP4_DMA_CONFIG_PACK_ALIGN_MSB |
                        MDP4_DMA_CONFIG_DEFLKR_EN |
                        MDP4_DMA_CONFIG_DITHER_EN |
                        MDP4_DMA_CONFIG_R_BPC(BPC8) |
                        MDP4_DMA_CONFIG_G_BPC(BPC8) |
                        MDP4_DMA_CONFIG_B_BPC(BPC8) |
                        MDP4_DMA_CONFIG_PACK(0x21));
        mdp4_crtc_set_intf(encoder->crtc, INTF_DSI_VIDEO,0);
	//mdp4_crtc_set_intf(encoder->crtc, INTF_DSI_CMD, 0);

	mdp4_dsi_encoder_dpms(encoder, DRM_MODE_DPMS_ON);
}

static const struct drm_encoder_helper_funcs mdp4_dsi_encoder_helper_funcs = {
	.dpms = mdp4_dsi_encoder_dpms,
	.mode_fixup = mdp4_dsi_encoder_mode_fixup,
	.mode_set = mdp4_dsi_encoder_mode_set,
	.prepare = mdp4_dsi_encoder_prepare,
	.commit = mdp4_dsi_encoder_commit,
};

long mdp4_dsi_round_pixclk(struct drm_encoder *encoder, unsigned long rate)
{
//	struct mdp4_dsi_encoder *mdp4_dsi_encoder = to_mdp4_dsi_encoder(encoder);
//	return clk_round_rate(mdp4_dsi_encoder->mdp_p_clk, rate);
	return 0;
}

/* initialize encoder */
struct drm_encoder *mdp4_dsi_encoder_init(struct drm_device *dev
			)
{
	struct drm_encoder *encoder = NULL;
	struct mdp4_dsi_encoder *mdp4_dsi_encoder;
	int ret;

	mdp4_dsi_encoder = kzalloc(sizeof(*mdp4_dsi_encoder), GFP_KERNEL);
	if (!mdp4_dsi_encoder) {
		ret = -ENOMEM;
		goto fail;
	}

//	mdp4_dsi_encoder->panel = panel;

	encoder = &mdp4_dsi_encoder->base;

	drm_encoder_init(dev, encoder, &mdp4_dsi_encoder_funcs,
			 DRM_MODE_ENCODER_DSI);
	drm_encoder_helper_add(encoder, &mdp4_dsi_encoder_helper_funcs);
	
	bs_init(mdp4_dsi_encoder);

	return encoder;

fail:
	if (encoder)
		mdp4_dsi_encoder_destroy(encoder);

	return ERR_PTR(ret);
}
