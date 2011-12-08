LOCAL_PATH := $(call my-dir)

# TODO It would be nice if we could just specify the modules we actually
# need and have some mechanism to pull in dependencies instead
# of explicitly enumerating everything that goes in
# use something like add-required-deps
droidboot_modules := \
	libc \
	libcutils \
	libdl \
	liblog \
	libm \
	libstdc++ \
	linker \
	mksh \
	systembinsh \
	toolbox \
	libdiskconfig \
	libext2fs \
	libext2_com_err \
	libext2_e2p \
	libext2_blkid \
	libext2_uuid \
	libext2_profile \
	libext4_utils \
	libusbhost \
	libz \
	resize2fs \
	tune2fs \
	e2fsck \
	gzip \
	kexec \
	droidboot \

droidboot_system_files = $(call module-installed-files,$(droidboot_modules))

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
	cp -fR $(srcfile) $$destfile; \
)
endef

droidboot_out := $(PRODUCT_OUT)/droidboot
droidboot_root_out := $(droidboot_out)/root
droidboot_data_out := $(droidboot_root_out)/data
droidboot_system_out := $(droidboot_root_out)/system
droidboot_etc_out := $(droidboot_system_out)/etc
droidboot_initrc := $(LOCAL_PATH)/init.rc

DROIDBOOT_RAMDISK := $(droidboot_out)/ramdisk-droidboot.img.gz
DROIDBOOT_BOOTIMAGE := $(PRODUCT_OUT)/droidboot.img

# Used by Droidboot to know what device the SD card is on for OTA
recovery_fstab := $(TARGET_DEVICE_DIR)/recovery.fstab

# NOTE: You'll need to pass g_android.fastboot=1 on the kernel command line
# so that the ADB driver exports the right protocol
$(DROIDBOOT_RAMDISK): \
		$(LOCAL_PATH)/ramdisk.mk \
		$(MKBOOTFS) \
		$(TARGET_DISK_LAYOUT_CONFIG) \
		$(INSTALLED_RAMDISK_TARGET) \
		$(INSTALLED_SYSTEMIMAGE) \
		$(MINIGZIP) \
		$(recovery_fstab) \
		$(droidboot_initrc) \
		$(droidboot_resources_deps) \
		$(droidboot_system_files) \

	$(hide) rm -rf $(droidboot_root_out)
	$(hide) mkdir -p $(droidboot_root_out)
	$(hide) mkdir -p $(droidboot_root_out)/sbin
	$(hide) mkdir -p $(droidboot_root_out)/data
	$(hide) mkdir -p $(droidboot_root_out)/mnt
	$(hide) mkdir -p $(droidboot_system_out)
	$(hide) mkdir -p $(droidboot_system_out)/etc
	$(hide) mkdir -p $(droidboot_system_out)/bin
	$(hide) cp -fR $(TARGET_ROOT_OUT) $(droidboot_out)
	$(hide) rm -f $(droidboot_root_out)/init*.rc
	$(hide) cp -f $(droidboot_initrc) $(droidboot_root_out)
ifneq ($(DROIDBOOT_NO_GUI),true)
	$(hide) cp -rf $(droidboot_resources_common) $(droidboot_root_out)/
endif
	$(hide) cp -f $(recovery_fstab) $(droidboot_etc_out)/recovery.fstab
	$(hide) $(call droidboot-copy-files,$(TARGET_OUT),$(droidboot_system_out))
	$(hide) cp -f $(TARGET_DISK_LAYOUT_CONFIG) $(droidboot_etc_out)/disk_layout.conf
	$(hide) $(MKBOOTFS) $(droidboot_root_out) | $(MINIGZIP) > $@
	@echo "Created Droidboot ramdisk: $@"

# Create a standard Android bootimage using the regular kernel and the
# droidboot ramdisk.
$(DROIDBOOT_BOOTIMAGE): \
		$(INSTALLED_KERNEL_TARGET) \
		$(DROIDBOOT_RAMDISK) \
		$(BOARD_KERNEL_CMDLINE_FILE) \
		$(MKBOOTIMG) \

	$(hide) $(MKBOOTIMG) --kernel $(INSTALLED_KERNEL_TARGET) \
		     --ramdisk $(DROIDBOOT_RAMDISK) \
		     --cmdline "g_android.fastboot=1 $(BOARD_KERNEL_CMDLINE)" \
		     --output $@
	@echo "Created Droidboot bootimage: $@"

.PHONY: droidboot-ramdisk
droidboot-ramdisk: $(DROIDBOOT_RAMDISK)

.PHONY: droidboot-bootimage
droidboot-bootimage: $(DROIDBOOT_BOOTIMAGE)

