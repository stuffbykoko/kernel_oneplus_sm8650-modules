KERNEL_SRC ?= /lib/modules/$(shell uname -r)/build

# The Make variable $(M) must point to the directory that contains the module
# source code (which includes this Makefile). It can either be an absolute or a
# relative path. If it is a relative path, then it must be relative to the
# kernel source directory (KERNEL_SRC). An absolute path can be obtained very
# easily through $(shell pwd). Generating a path relative to KERNEL_SRC is
# difficult and we accept some outside help by letting the caller override the
# variable $(M). Allowing a relative path for $(M) enables us to have the build
# system put output/object files (.o, .ko.) into a directory different from the
# module source directory.
M ?= $(shell pwd)

ifeq ($(WLAN_ROOT),)
WLAN_CHIPSET := qca_cld3
LOCAL_DEV_NAME := $(patsubst .%,%,\
	$(lastword $(strip $(subst /, ,$(M)))))

KBUILD_OPTIONS += \
    WLAN_ROOT=$(KERNEL_SRC)/$(M) \
    CONFIG_QCA_CLD_WLAN=m \
    CONFIG_CNSS_OUT_OF_TREE=y \
    CONFIG_IPA_OUT_OF_TREE=y \
    CONFIG_CNSS2=m \
    CONFIG_ICNSS2=m \
    CONFIG_CNSS2_QMI=y \
    CONFIG_ICNSS2_QMI=y \
    CONFIG_CNSS_QMI_SVC=m \
    CONFIG_CNSS_PLAT_IPC_QMI_SVC=m \
    CONFIG_CNSS_GENL=m \
    CONFIG_WCNSS_MEM_PRE_ALLOC=m \
    CONFIG_CNSS_UTILS=m \
    CONFIG_CNSS_HW_SECURE_DISABLE=y \
    CONFIG_BUS_AUTO_SUSPEND=y \
    BOARD_PLATFORM=$(TARGET_BOARD_PLATFORM) \
    WLAN_PROFILE=$(LOCAL_DEV_NAME) \
    DYNAMIC_SINGLE_CHIP=$(LOCAL_DEV_NAME) \
    MODNAME=$(WLAN_CHIPSET)_$(LOCAL_DEV_NAME) \
    DEVNAME=$(LOCAL_DEV_NAME) \
    WLAN_CTRL_NAME=wlan

ifeq ($(TARGET_BOARD_PLATFORM), volcano)
KBUILD_OPTIONS += CONFIG_QCA_CLD_WLAN_PROFILE=pineapple_gki_$(LOCAL_DEV_NAME)
else
KBUILD_OPTIONS += CONFIG_QCA_CLD_WLAN_PROFILE=$(TARGET_BOARD_PLATFORM)_gki_$(LOCAL_DEV_NAME)
endif

endif

KBUILD_EXTRA_SYMBOLS := \
    $(OUT_DIR)/../sm8650-modules/qcom/opensource/wlan/platform/Module.symvers \
    $(OUT_DIR)/../sm8650-modules/qcom/opensource/dataipa/drivers/platform/msm/Module.symvers

all:
	$(MAKE) -C $(KERNEL_SRC) M=$(M) modules $(KBUILD_OPTIONS)

modules_install:
	$(MAKE) INSTALL_MOD_STRIP=1 M=$(M) -C $(KERNEL_SRC) modules_install

clean:
	$(MAKE) -C $(KERNEL_SRC) M=$(M) clean $(KBUILD_OPTIONS)
