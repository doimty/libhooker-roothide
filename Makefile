THEOS_PACKAGE_SCHEME ?= roothide
THEOS_DEVICE_IP ?= 15
FINALPACKAGE ?= 1

ifeq ($(THEOS_PACKAGE_SCHEME),roothide)
include roothide.mk
else
# 显式清空包方案时保留上游构建，避免混入 roothide 安装布局。
override _THEOS_DEB_PACKAGE_CONTROL_PATH := $(CURDIR)/packaging/legacy-control
THEOS_LAYOUT_DIR_NAME = packaging/legacy-layout
override THEOS_LIBRARY_PATH := $(CURDIR)/worker/link-libraries
export CLANG_MODULE_CACHE_PATH := $(CURDIR)/worker/clang-module-cache
TARGET = iphone:latest:11.0
ARCHS=arm64 arm64e
include $(THEOS)/makefiles/common.mk

# 这些运行时安装的第三方库不属于系统共享缓存，兼容新版 Apple 链接器。
ADDITIONAL_LDFLAGS += -Wl,-not_for_dyld_shared_cache

LIBRARY_NAME = libhooker libblackjack libsubstitute libsubstrate

libhooker_FILES = libhooker/dyld/dyldSyms.c libhooker/dyld/dyld4Find.c libhooker/misc/misc.c libhooker/as-aarch64/as-aarch64.c libhooker/disas-aarch64/disas-aarch64.c libhooker/mem/writeMem-darwin.c libhooker/mem/shadowMem.c libhooker/mem/shadowMem.cpp libhooker/func/funcHook.c libhooker/mach/machWrapper.c
libhooker_CFLAGS = -Ilibhooker/include/apple -Ilibhooker/include/ -DTHEOS -fvisibility=hidden
libhooker_CCFLAGS = -Ilibhooker/include/apple -Ilibhooker/include/ -DTHEOS -fvisibility=hidden -std=c++11
libhooker_LINKAGE_TYPE = both

libblackjack_FILES = libblackjack/objc/objc.c libhooker/as-aarch64/as-aarch64.c
libblackjack_CFLAGS = -Ilibhooker/include/apple -Ilibhooker/include/ -fvisibility=hidden
libblackjack_LDFLAGS = -lobjc -lhooker -L$(THEOS_OBJ_DIR)

libsubstitute_FILES = substitute-shim/substitute-shim.c
libsubstitute_CFLAGS = -Ilibhooker/include/ -Ilibblackjack/include/
libsubstitute_LDFLAGS = -lhooker -lblackjack -L$(THEOS_OBJ_DIR)

libsubstrate_FILES = substrate-shim/substrate-shim.c
libsubstrate_CFLAGS = -Ilibhooker/include/ -Ilibblackjack/include/
libsubstrate_LDFLAGS = -lhooker -lblackjack -L$(THEOS_OBJ_DIR) -lobjc

include $(THEOS_MAKE_PATH)/library.mk

TOOL_NAME = libhookerTest
libhookerTest_FILES = $(libhooker_FILES) $(libblackjack_FILES) unittest/main.c
libhookerTest_CFLAGS = -Ilibhooker/include/apple -Ilibhooker/include/ -DTHEOS -fvisibility=hidden
libhookerTest_CCFLAGS = -Ilibhooker/include/apple -Ilibhooker/include/ -DTHEOS -fvisibility=hidden -std=c++11
libhookerTest_LDFLAGS = -lobjc
libhookerTest_CODESIGN_FLAGS = -Sunittest/webcontent.xml

include $(THEOS_MAKE_PATH)/tool.mk
endif
