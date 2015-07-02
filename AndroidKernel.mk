#Android makefile to build kernel as a part of Android Build
PERL		= perl

ifeq ($(TARGET_PREBUILT_KERNEL),)

KERNEL_OUT := $(TARGET_OUT_INTERMEDIATES)/KERNEL_OBJ
KERNEL_CONFIG := $(KERNEL_OUT)/.config
TARGET_PREBUILT_INT_KERNEL := $(KERNEL_OUT)/arch/arm/boot/zImage
KERNEL_IMG=$(KERNEL_OUT)/arch/arm/boot/Image

TARGET_PREBUILT_KERNEL := $(TARGET_PREBUILT_INT_KERNEL).dtb

REALTOP := $(realpath $(TOP))
KERNEL_COMPILER_PATH := $(REALTOP)/prebuilts/gcc/linux-x86/arm/arm-eabi-4.8/bin/

#CROSS=arm-linux-gnueabi-
CROSS=arm-eabi-

$(KERNEL_OUT):
	mkdir -p $(KERNEL_OUT)

$(KERNEL_CONFIG): $(KERNEL_OUT)
	export PATH=$(KERNEL_COMPILER_PATH):$(PATH) &&\
	$(MAKE) -C kernel/flo O=../../$(KERNEL_OUT) ARCH=arm CROSS_COMPILE=$(CROSS) $(KERNEL_DEFCONFIG)


$(TARGET_PREBUILT_KERNEL): $(KERNEL_OUT) $(KERNEL_CONFIG)
	export PATH=$(KERNEL_COMPILER_PATH):$(PATH) &&\
	$(MAKE) -C kernel/flo O=../../$(KERNEL_OUT) ARCH=arm CROSS_COMPILE=$(CROSS)
	export PATH=$(KERNEL_COMPILER_PATH):$(PATH) &&\
	$(MAKE) -C kernel/flo O=../../$(KERNEL_OUT) ARCH=arm CROSS_COMPILE=$(CROSS) qcom-apq8064-asus-nexus7-flo.dtb
	cat $(TARGET_PREBUILT_INT_KERNEL) $(KERNEL_OUT)/arch/arm/boot/dts/qcom-apq8064-asus-nexus7-flo.dtb > $(TARGET_PREBUILT_KERNEL)

endif
