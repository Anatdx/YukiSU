LOCAL_PATH := $(call my-dir)

include $(CLEAR_VARS)
LOCAL_MODULE := su
LOCAL_SRC_FILES := su.c
# Size optimization (Oz + LTO)
LOCAL_CFLAGS := -Oz -DNDEBUG -ffunction-sections -fdata-sections -flto
LOCAL_LDFLAGS := -Wl,--gc-sections -Wl,--strip-all -flto
include $(BUILD_EXECUTABLE)
