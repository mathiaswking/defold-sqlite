LOCAL_PATH := $(call my-dir)
LOCAL_SRC_FILES := sqlite3.c
LOCAL_MODULE    := sqlite3
LOCAL_MODULE_FILENAME := libsqlite3
include $(BUILD_STATIC_LIBRARY)
TARGET_PLATFORM := android-14
TARGET_ARCH_ABI := armeabi-v7a

