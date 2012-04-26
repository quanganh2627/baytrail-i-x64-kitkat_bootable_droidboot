LOCAL_PATH := $(call my-dir)
include $(CLEAR_VARS)

ifeq ($(TARGET_USE_DROIDBOOT),true)

LOCAL_SRC_FILES := \
	aboot.c \
	fastboot.c \
	util.c \
	droidboot.c
DROIDBOOT_OTA_UPDATE_FILE ?= /cache/update.zip
LOCAL_CFLAGS := -DDEVICE_NAME=\"$(TARGET_BOOTLOADER_BOARD_NAME)\" \
	-W -Wall -Wno-unused-parameter -Werror \
	-D OTA_UPDATE_FILE='"$(DROIDBOOT_OTA_UPDATE_FILE)"'

LOCAL_MODULE := droidboot
LOCAL_MODULE_TAGS := eng
LOCAL_SHARED_LIBRARIES := liblog libext4_utils libz
LOCAL_STATIC_LIBRARIES += libcharger libminui libmtdutils libpng libpixelflinger_static libc libcutils libvolumeutils libmtdutils libpower
LOCAL_STATIC_LIBRARIES += $(TARGET_DROIDBOOT_LIBS) $(TARGET_DROIDBOOT_EXTRA_LIBS)
LOCAL_C_INCLUDES += bootable/recovery system/extras/ext4_utils bionic/libc/private external/zlib

#libpixelflinger_static for x86 is using encoder under hardware/intel/apache-harmony
ifeq ($(TARGET_ARCH),x86)
LOCAL_STATIC_LIBRARIES += libenc
endif

# Each library in TARGET_DROIDBOOT_LIBS should have a function
# named "<libname>_init()".  Here we emit a little C function that
# gets #included by aboot.c.  It calls all those registration
# functions.

# Devices can also add libraries to TARGET_DROIDBOOT_EXTRA_LIBS.
# These libs are also linked in with droidboot, but we don't try to call
# any sort of registration function for these.  Use this variable for
# any subsidiary static libraries required for your registered
# extension libs.

inc := $(call intermediates-dir-for,PACKAGING,droidboot_extensions)/register.inc

# During the first pass of reading the makefiles, we dump the list of
# extension libs to a temp file, then copy that to the ".list" file if
# it is different than the existing .list (if any).  The register.inc
# file then uses the .list as a prerequisite, so it is only rebuilt
# (and aboot.o recompiled) when the list of extension libs changes.

junk := $(shell mkdir -p $(dir $(inc));\
	        echo $(TARGET_DROIDBOOT_LIBS) > $(inc).temp;\
	        diff -q $(inc).temp $(inc).list 2>/dev/null || cp -f $(inc).temp $(inc).list)

$(inc) : libs := $(TARGET_DROIDBOOT_LIBS)
$(inc) : $(inc).list $(LOCAL_PATH)/Android.mk
	$(hide) mkdir -p $(dir $@)
	$(hide) echo "" > $@
	$(hide) $(foreach lib,$(libs),echo "extern void $(lib)_init(void);" >> $@)
	$(hide) echo "void register_droidboot_plugins() {" >> $@
	$(hide) $(foreach lib,$(libs),echo "  $(lib)_init();" >> $@)
	$(hide) echo "}" >> $@

$(call intermediates-dir-for,EXECUTABLES,droidboot)/aboot.o : $(inc)
LOCAL_C_INCLUDES += $(dir $(inc))

ifneq ($(DROIDBOOT_NO_GUI),true)
LOCAL_SRC_FILES += ui.c
LOCAL_CFLAGS += -DUSE_GUI
endif

include $(BUILD_EXECUTABLE)

endif # TARGET_USE_DROIDBOOT
