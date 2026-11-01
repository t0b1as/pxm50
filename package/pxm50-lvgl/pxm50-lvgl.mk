################################################################################
# pxm50-lvgl – lädt nur die LVGL-Quellen, gebaut wird in pxm50-panel
################################################################################
PXM50_LVGL_VERSION = 9.2.2
PXM50_LVGL_SITE = $(call github,lvgl,lvgl,v$(PXM50_LVGL_VERSION))
PXM50_LVGL_LICENSE = MIT
PXM50_LVGL_LICENSE_FILES = LICENCE.txt
PXM50_LVGL_INSTALL_TARGET = NO
$(eval $(generic-package))
