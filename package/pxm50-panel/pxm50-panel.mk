################################################################################
# pxm50-panel – LVGL-HA-Panel (Quellen lokal, LVGL aus Paket pxm50-lvgl)
################################################################################
PXM50_PANEL_VERSION = 0.7.2
PXM50_PANEL_SITE = $(BR2_EXTERNAL_PXM50_PATH)/src/pxm50-panel
PXM50_PANEL_SITE_METHOD = local
PXM50_PANEL_LICENSE = MIT
PXM50_PANEL_DEPENDENCIES = pxm50-lvgl libcurl cjson jpeg tslib

define PXM50_PANEL_BUILD_CMDS
	rm -rf $(@D)/lvgl
	rsync -a --exclude='.stamp_*' --exclude='.files-list*' \
		$(PXM50_LVGL_DIR)/ $(@D)/lvgl/
	$(TARGET_MAKE_ENV) $(MAKE) -C $(@D) CC="$(TARGET_CC)" \
		CFLAGS="$(TARGET_CFLAGS)" LDFLAGS="$(TARGET_LDFLAGS)"
endef

define PXM50_PANEL_INSTALL_TARGET_CMDS
	$(INSTALL) -D -m 0755 $(@D)/pxm50-panel $(TARGET_DIR)/usr/bin/pxm50-panel
endef

$(eval $(generic-package))
