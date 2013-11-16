LOCAL_PATH := $(call my-dir)

# TODO It would be nice if we could just specify the modules we actually
# need and have some mechanism to pull in dependencies instead
# of explicitly enumerating everything that goes in
# use something like add-required-deps
droidboot_modules := \
	libc \
	libcutils \
	libnetutils \
	libdl \
	liblog \
	libm \
	libstdc++ \
	linker \
	mksh \
	systembinsh \
	toolbox \
	libext2fs \
	libext2_com_err \
	libext2_e2p \
	libext2_blkid \
	libext2_uuid \
	libext2_profile \
	libext4_utils \
	libsparse \
	libusbhost \
	libz \
	resize2fs \
	tune2fs \
	e2fsck \
	gzip \
	droidboot \
	netcfg \
	init.utilitynet.sh \
	simg2img \
	dhcpcd \

droidboot_system_files = $(filter $(PRODUCT_OUT)%,$(call module-installed-files,$(droidboot_modules)))

ifneq ($(DROIDBOOT_NO_GUI),true)
droidboot_resources_common := $(LOCAL_PATH)/res
droidboot_resources_deps := $(shell find $(droidboot_resources_common) -type f)
endif

# $(1): source base dir
# $(2): target base dir
define droidboot-copy-files
$(hide) $(foreach srcfile,$(droidboot_system_files), \
	destfile=$(patsubst $(1)/%,$(2)/%,$(srcfile)); \
	mkdir -p `dirname $$destfile`; \
	$(ACP) -fdp $(srcfile) $$destfile; \
)
endef

droidboot_out := $(PRODUCT_OUT)/droidboot
DROIDBOOT_ROOT_OUT := $(droidboot_out)/root
droidboot_data_out := $(DROIDBOOT_ROOT_OUT)/data
droidboot_system_out := $(DROIDBOOT_ROOT_OUT)/system
droidboot_etc_out := $(droidboot_system_out)/etc
droidboot_initrc := $(LOCAL_PATH)/init.rc

DROIDBOOT_RAMDISK := $(droidboot_out)/ramdisk-droidboot.img.gz
DROIDBOOT_BOOTIMAGE := $(PRODUCT_OUT)/droidboot.img

# Used by Droidboot to know what device the SD card is on for OTA
ifdef TARGET_RECOVERY_FSTAB
recovery_fstab := $(TARGET_RECOVERY_FSTAB)
else
recovery_fstab := $(TARGET_DEVICE_DIR)/recovery.fstab
endif

# NOTE: You'll need to pass g_android.fastboot=1 on the kernel command line
# so that the ADB driver exports the right protocol
$(DROIDBOOT_RAMDISK): \
		$(LOCAL_PATH)/ramdisk.mk \
		$(MKBOOTFS) \
		$(INSTALLED_RAMDISK_TARGET) \
		$(INSTALLED_SYSTEMIMAGE) \
		$(MINIGZIP) \
		$(recovery_fstab) \
		$(droidboot_initrc) \
		$(DROIDBOOT_HARDWARE_INITRC) \
		$(droidboot_resources_deps) \
		$(droidboot_system_files) \

	$(hide) rm -rf $(DROIDBOOT_ROOT_OUT)
	$(hide) mkdir -p $(DROIDBOOT_ROOT_OUT)
	$(hide) mkdir -p $(DROIDBOOT_ROOT_OUT)/sbin
	$(hide) mkdir -p $(DROIDBOOT_ROOT_OUT)/data
	$(hide) mkdir -p $(DROIDBOOT_ROOT_OUT)/mnt
	$(hide) mkdir -p $(droidboot_system_out)
	$(hide) mkdir -p $(droidboot_system_out)/etc
	$(hide) mkdir -p $(droidboot_system_out)/bin
	$(hide) $(ACP) -fr $(TARGET_ROOT_OUT) $(droidboot_out)
	$(hide) rm -f $(DROIDBOOT_ROOT_OUT)/init*.rc
	$(hide) $(ACP) -f $(droidboot_initrc) $(DROIDBOOT_ROOT_OUT)
ifneq ($(strip $(DROIDBOOT_HARDWARE_INITRC)),)
	$(hide) $(ACP) -f $(DROIDBOOT_HARDWARE_INITRC) $(DROIDBOOT_ROOT_OUT)/init.droidboot.rc
endif
ifneq ($(DROIDBOOT_NO_GUI),true)
	$(hide) $(ACP) -rf $(droidboot_resources_common) $(DROIDBOOT_ROOT_OUT)/
endif
	$(hide) $(ACP) -f $(recovery_fstab) $(droidboot_etc_out)/recovery.fstab
	$(hide) $(call droidboot-copy-files,$(TARGET_OUT),$(droidboot_system_out))
	$(hide) $(MKBOOTFS) $(DROIDBOOT_ROOT_OUT) | $(MINIGZIP) > $@
	@echo "Created Droidboot ramdisk: $@"

DROIDBOOT_CMDLINE := g_android.fastboot=1
ifneq ($(DROIDBOOT_SCRATCH_SIZE),)
DROIDBOOT_CMDLINE += droidboot.scratch=$(DROIDBOOT_SCRATCH_SIZE)
endif
DROIDBOOT_CMDLINE += $(BOARD_KERNEL_CMDLINE)

# Create a standard Android bootimage using the regular kernel and the
# droidboot ramdisk.
$(DROIDBOOT_BOOTIMAGE): \
		$(INSTALLED_KERNEL_TARGET) \
		$(DROIDBOOT_RAMDISK) \
		$(BOARD_KERNEL_CMDLINE_FILE) \
		$(MKBOOTIMG) \

	$(hide) $(MKBOOTIMG) --kernel $(INSTALLED_KERNEL_TARGET) \
		     --ramdisk $(DROIDBOOT_RAMDISK) \
		     --cmdline "$(DROIDBOOT_CMDLINE)" \
		     $(BOARD_MKBOOTIMG_ARGS) \
		     --output $@
	@echo "Created Droidboot bootimage: $@"

.PHONY: droidboot-ramdisk
droidboot-ramdisk: $(DROIDBOOT_RAMDISK)

.PHONY: droidboot-bootimage
droidboot-bootimage: $(DROIDBOOT_BOOTIMAGE)

.PHONY: droidbootimage
droidbootimage: $(DROIDBOOT_BOOTIMAGE)

