ccflags-y := -Iinclude/drm -Idrivers/gpu/drm/msm -I$(src) -include reg_log.h
ifeq (, $(findstring -W,$(EXTRA_CFLAGS)))
	ccflags-y += -Werror
endif

obj-y += pwm_bl.o

msm-y := \
	adreno/adreno_gpu.o \
	adreno/a3xx_gpu.o \
	dsi/dsi.o \
        dsi/dsi_bridge.o \
        dsi/dsi_clk.o \
        dsi/dsi_connector.o \
        dsi/dsi_mipi.o \
        dsi/dsi_phy_8960.o \
        dsi/dsi_phy_8x60.o \
	hdmi/hdmi.o \
	hdmi/hdmi_audio.o \
	hdmi/hdmi_bridge.o \
	hdmi/hdmi_connector.o \
	hdmi/hdmi_i2c.o \
	hdmi/hdmi_phy_8960.o \
	hdmi/hdmi_phy_8x60.o \
	hdmi/hdmi_phy_8x74.o \
	mdp/mdp_format.o \
	mdp/mdp_kms.o \
	mdp/mdp4/mdp4_crtc.o \
	mdp/mdp4/mdp4_dtv_encoder.o \
        mdp/mdp4/mdp4_dsi_encoder.o \
	mdp/mdp4/mdp4_irq.o \
	mdp/mdp4/mdp4_kms.o \
	mdp/mdp4/mdp4_plane.o \
        panel/panel.o \
        panel/panel_jdi_1080p_cmd.o \
        panel/panel_truly.o \
	mdp/mdp5/mdp5_crtc.o \
	mdp/mdp5/mdp5_encoder.o \
	mdp/mdp5/mdp5_irq.o \
	mdp/mdp5/mdp5_kms.o \
	mdp/mdp5/mdp5_plane.o \
	mdp/mdp5/mdp5_smp.o \
	msm_drv.o \
	msm_fb.o \
	msm_gem.o \
	msm_gem_prime.o \
	msm_gem_submit.o \
	msm_gpu.o \
	msm_iommu.o \
	msm_perf.o \
	msm_rd.o \
	msm_ringbuffer.o

msm-$(CONFIG_DRM_MSM_LVDS) +=   mdp/mdp4/mdp4_lcdc_encoder.o \
        mdp/mdp4/mdp4_lvds_connector.o \
        mdp/mdp4/mdp4_lvds_pll.o
 
msm-$(CONFIG_DRM_MSM_FBDEV) += msm_fbdev.o

obj-$(CONFIG_DRM_MSM)	+= msm.o
