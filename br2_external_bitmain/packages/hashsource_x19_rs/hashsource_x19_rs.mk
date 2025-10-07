################################################################################
#
# hashsource_x19_rs
#
################################################################################

HASHSOURCE_X19_RS_VERSION = 1.0.0
HASHSOURCE_X19_RS_SITE = $(BR2_EXTERNAL_X19_BITMAIN_PATH)/../hashsource_x19_rs
HASHSOURCE_X19_RS_SITE_METHOD = local
HASHSOURCE_X19_RS_LICENSE = GPL-2.0
HASHSOURCE_X19_RS_LICENSE_FILES = LICENSE

# Build all binaries in release mode
# Buildroot automatically handles:
# - Cross-compilation to ARM (armv7-unknown-linux-gnueabihf)
# - Size optimization (BR2_OPTIMIZE_S -> opt-level="s")
# - LTO (BR2_ENABLE_LTO -> lto=true)
# - Debug symbols stripping
HASHSOURCE_X19_RS_CARGO_BUILD_OPTS = --bins

# Auto-vendor Cargo dependencies if not already present
# This allows local development without committing VENDOR/ to git
define HASHSOURCE_X19_RS_VENDOR_DEPENDENCIES
	if [ ! -d $(@D)/VENDOR ]; then \
		echo "Vendoring Rust dependencies for offline build..."; \
		mkdir -p $(@D)/.cargo; \
		cd $(@D) && $(PKG_CARGO_ENV) cargo vendor --locked VENDOR; \
		printf '[source.crates-io]\nreplace-with = "vendored-sources"\n\n[source.vendored-sources]\ndirectory = "VENDOR"\n' > $(@D)/.cargo/config.toml; \
	fi
endef

HASHSOURCE_X19_RS_PRE_BUILD_HOOKS += HASHSOURCE_X19_RS_VENDOR_DEPENDENCIES

# Installation - binaries are in target/$(RUSTC_TARGET_NAME)/release/
# define HASHSOURCE_X19_RS_INSTALL_TARGET_CMDS
# 	$(INSTALL) -D -m 0755 $(@D)/target/$(RUSTC_TARGET_NAME)/release/fan_control \
# 		$(TARGET_DIR)/usr/bin/fan_control
# endef

# Use Buildroot's cargo-package infrastructure
$(eval $(cargo-package))
