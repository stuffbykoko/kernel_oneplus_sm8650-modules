# SPDX-License-Identifier: GPL-2.0

DISPLAY_SELECT := CONFIG_DRM_MSM=m

KBUILD_OPTIONS += DISPLAY_ROOT=$(KERNEL_SRC)/$(M)/../
KBUILD_OPTIONS += BOARD_PLATFORM=$(TARGET_BOARD_PLATFORM)
KBUILD_OPTIONS += $(DISPLAY_SELECT)

KBUILD_EXTRA_SYMBOLS := \
    $(OUT_DIR)/../sm8650-modules/qcom/opensource/mm-drivers/msm_ext_display/Module.symvers \
    $(OUT_DIR)/../sm8650-modules/qcom/opensource/mm-drivers/sync_fence/Module.symvers \
    $(OUT_DIR)/../sm8650-modules/qcom/opensource/securemsm-kernel/Module.symvers

ifneq ($(BOARD_PLATFORM), volcano)
KBUILD_EXTRA_SYMBOLS += \
    $(OUT_DIR)/../sm8650-modules/qcom/opensource/mm-drivers/hw_fence/Module.symvers \
    $(OUT_DIR)/../sm8650-modules/qcom/opensource/mmrm-driver/Module.symvers
endif

all: modules

modules_install:
	$(MAKE) INSTALL_MOD_STRIP=1 -C $(KERNEL_SRC) M=$(M) modules_install

%:
	$(MAKE) -C $(KERNEL_SRC) M=$(M) $@ $(KBUILD_OPTIONS) $(KBUILD_EXTRA_SYMBOLS)

clean:
	rm -f *.o *.ko *.mod.c *.mod.o *~ .*.cmd Module.symvers
	rm -rf .tmp_versions

