################################################################################
#
# zg2332m-plugin
#
# Majestic AE plugin tailored for the Zosi ZG2332M (Hi3516EV100 + SC2235P).
# Ships /usr/lib/hisilicon.so + /etc/majestic-ae.conf.
#
# Not built from the openipc/majestic-plugins repo because that repo's
# hisilicon target uses cv500-family MPP APIs that do not exist on cv300.
# The plugin.c/plugin.h glue is a tiny 30-line stub and is shipped inline
# under src/.
#
################################################################################

ZG2332M_PLUGIN_VERSION = 1.0
ZG2332M_PLUGIN_SITE = $(BR2_EXTERNAL_GENERAL_PATH)/package/zg2332m-plugin/src
ZG2332M_PLUGIN_SITE_METHOD = local
ZG2332M_PLUGIN_LICENSE = MIT

# hisilicon-opensdk pulls openipc/openhisilicon which provides the MPP
# headers under kernel/include/<soc-family>/ — that is what we track for
# API updates.
ZG2332M_PLUGIN_DEPENDENCIES = hisilicon-opensdk majestic

ZG2332M_PLUGIN_MPP_INCDIR = $(HISILICON_OPENSDK_DIR)/kernel/include/$(OPENIPC_SOC_FAMILY)

define ZG2332M_PLUGIN_BUILD_CMDS
	$(TARGET_CC) $(TARGET_CFLAGS) \
		$(@D)/custom.c \
		$(@D)/plugin.c \
		-I$(@D) \
		-I$(ZG2332M_PLUGIN_MPP_INCDIR) \
		-o $(@D)/hisilicon.so \
		-Os -s -shared -fPIC -Wall
endef

define ZG2332M_PLUGIN_INSTALL_TARGET_CMDS
	$(INSTALL) -m 644 -D $(@D)/hisilicon.so \
		$(TARGET_DIR)/usr/lib/hisilicon.so
	$(INSTALL) -m 644 -D $(@D)/majestic-ae.conf \
		$(TARGET_DIR)/etc/majestic-ae.conf
	# Enable plugin loading in the majestic config that the majestic
	# package installed just before us.
	if [ -f $(TARGET_DIR)/etc/majestic.yaml ] && \
	   ! grep -q '^plugins: true' $(TARGET_DIR)/etc/majestic.yaml; then \
		echo 'plugins: true' >> $(TARGET_DIR)/etc/majestic.yaml; \
	fi
endef

$(eval $(generic-package))
