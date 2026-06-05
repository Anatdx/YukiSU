LOCAL_PATH := $(call my-dir)

include $(CLEAR_VARS)
LOCAL_MODULE := su
LOCAL_SRC_FILES := su.c
# Size optimization (Os + LTO)
LOCAL_CFLAGS := -Os -DNDEBUG -ffunction-sections -fdata-sections -flto
LOCAL_LDFLAGS := -Wl,--gc-sections -Wl,--strip-all -flto
include $(BUILD_EXECUTABLE)
