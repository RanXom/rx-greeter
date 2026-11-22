# rx-greeter - lightweight Wayland greeter for greetd
#
# Common targets:
#   make                 build build/rx-greeter and build/rx-greeter-power
#   make check           build and run the unit tests
#   make integration     run the nested headless Sway + mock greetd test
#   make install         install into $(PREFIX) (default /usr/local)
#   make install-power-setuid
#                        install, then make the power helper setuid root
#
# All dependencies are discovered through pkg-config. The only build-time
# generated sources are the xdg-shell protocol files from wayland-protocols.

CC              ?= cc
PKG_CONFIG      ?= pkg-config
WAYLAND_SCANNER ?= wayland-scanner
INSTALL         ?= install
PYTHON          ?= python3

VERSION     ?= 1.0.0

PREFIX      ?= /usr/local
BINDIR      ?= $(PREFIX)/bin
LIBEXECDIR  ?= $(PREFIX)/libexec
SHAREDIR    ?= $(PREFIX)/share
SYSCONFDIR  ?= /etc
LOCALSTATEDIR ?= /var/lib
POWER_GROUP ?= greetd

PKGS = wayland-client cairo xkbcommon json-c fontconfig
PKG_CFLAGS := $(shell $(PKG_CONFIG) --cflags $(PKGS))
PKG_LIBS   := $(shell $(PKG_CONFIG) --libs $(PKGS))
WAYLAND_PROTOCOLS_DIR := $(shell $(PKG_CONFIG) --variable=pkgdatadir wayland-protocols)
ifeq ($(WAYLAND_PROTOCOLS_DIR),)
WAYLAND_PROTOCOLS_DIR := /usr/share/wayland-protocols
endif
XDG_SHELL_XML := $(WAYLAND_PROTOCOLS_DIR)/stable/xdg-shell/xdg-shell.xml
LAYER_SHELL_XML := protocols/wlr-layer-shell-unstable-v1.xml
FRACTIONAL_SCALE_XML := $(WAYLAND_PROTOCOLS_DIR)/staging/fractional-scale/fractional-scale-v1.xml
VIEWPORTER_XML := $(WAYLAND_PROTOCOLS_DIR)/stable/viewporter/viewporter.xml

WARNINGS = -Wall -Wextra -Wshadow -Wformat=2 -Wpointer-arith \
           -Wstrict-prototypes -Wmissing-prototypes -Wno-unused-parameter
CFLAGS  ?= -O2 -g
override CFLAGS  += -std=c11 $(WARNINGS)
override CPPFLAGS += -D_GNU_SOURCE -DRX_GREETER_VERSION=\"$(VERSION)\" $(PKG_CFLAGS) -Ibuild/protocol
override LDLIBS  += $(PKG_LIBS) -lm

BUILD := build
OBJDIR := $(BUILD)/obj
GEN_DIR := $(BUILD)/protocol
XDG_HEADER := $(GEN_DIR)/xdg-shell-client-protocol.h
XDG_CODE   := $(GEN_DIR)/xdg-shell-protocol.c
XDG_OBJECT := $(OBJDIR)/xdg-shell-protocol.o
LAYER_HEADER := $(GEN_DIR)/wlr-layer-shell-unstable-v1-client-protocol.h
LAYER_CODE   := $(GEN_DIR)/wlr-layer-shell-unstable-v1-protocol.c
LAYER_OBJECT := $(OBJDIR)/wlr-layer-shell-unstable-v1-protocol.o
FRACTIONAL_HEADER := $(GEN_DIR)/fractional-scale-v1-client-protocol.h
FRACTIONAL_CODE   := $(GEN_DIR)/fractional-scale-v1-protocol.c
FRACTIONAL_OBJECT := $(OBJDIR)/fractional-scale-v1-protocol.o
VIEWPORTER_HEADER := $(GEN_DIR)/viewporter-client-protocol.h
VIEWPORTER_CODE   := $(GEN_DIR)/viewporter-protocol.c
VIEWPORTER_OBJECT := $(OBJDIR)/viewporter-protocol.o
PROTOCOL_OBJECTS := $(XDG_OBJECT) $(LAYER_OBJECT) $(FRACTIONAL_OBJECT) $(VIEWPORTER_OBJECT)

CORE_SRC := src/config.c src/greetd.c src/password.c src/sessions.c \
            src/ui.c src/users.c src/util.c
CORE_OBJ := $(patsubst src/%.c,$(OBJDIR)/%.o,$(CORE_SRC))
MAIN_OBJ := $(OBJDIR)/main.o
POWER_SRC := src/rx-greeter-power.c
POWER_OBJ := $(OBJDIR)/rx-greeter-power.o

TARGET := $(BUILD)/rx-greeter
POWER_TARGET := $(BUILD)/rx-greeter-power

UNIT_SRC := tests/unit/unit.c
UNIT_OBJ := $(OBJDIR)/tests/unit.o
UNIT_TARGET := $(BUILD)/unit-tests

TEST_TOOL := $(BUILD)/wl-input
TEST_PROTO_DIR := $(BUILD)/test-protocol
TEST_PROTOCOLS := \
	$(TEST_PROTO_DIR)/virtual-keyboard-unstable-v1-client-protocol.h \
	$(TEST_PROTO_DIR)/virtual-keyboard-unstable-v1-protocol.c \
	$(TEST_PROTO_DIR)/wlr-virtual-pointer-unstable-v1-client-protocol.h \
	$(TEST_PROTO_DIR)/wlr-virtual-pointer-unstable-v1-protocol.c

.PHONY: all clean check test integration multioutput install install-power-setuid \
        install-system-config uninstall

all: $(TARGET) $(POWER_TARGET)

$(BUILD) $(OBJDIR) $(GEN_DIR) $(TEST_PROTO_DIR):
	mkdir -p $@

$(XDG_HEADER): $(XDG_SHELL_XML) | $(GEN_DIR)
	$(WAYLAND_SCANNER) client-header $< $@

$(XDG_CODE): $(XDG_SHELL_XML) | $(GEN_DIR)
	$(WAYLAND_SCANNER) private-code $< $@

$(XDG_OBJECT): $(XDG_CODE) $(XDG_HEADER) | $(OBJDIR)
	$(CC) $(CFLAGS) $(CPPFLAGS) -c $< -o $@

$(LAYER_HEADER): $(LAYER_SHELL_XML) | $(GEN_DIR)
	$(WAYLAND_SCANNER) client-header $< $@

$(LAYER_CODE): $(LAYER_SHELL_XML) | $(GEN_DIR)
	$(WAYLAND_SCANNER) private-code $< $@

$(FRACTIONAL_HEADER): $(FRACTIONAL_SCALE_XML) | $(GEN_DIR)
	$(WAYLAND_SCANNER) client-header $< $@

$(FRACTIONAL_CODE): $(FRACTIONAL_SCALE_XML) | $(GEN_DIR)
	$(WAYLAND_SCANNER) private-code $< $@

$(VIEWPORTER_HEADER): $(VIEWPORTER_XML) | $(GEN_DIR)
	$(WAYLAND_SCANNER) client-header $< $@

$(VIEWPORTER_CODE): $(VIEWPORTER_XML) | $(GEN_DIR)
	$(WAYLAND_SCANNER) private-code $< $@

$(LAYER_OBJECT): $(LAYER_CODE) $(LAYER_HEADER) | $(OBJDIR)
	$(CC) $(CFLAGS) $(CPPFLAGS) -c $< -o $@

$(FRACTIONAL_OBJECT): $(FRACTIONAL_CODE) $(FRACTIONAL_HEADER) | $(OBJDIR)
	$(CC) $(CFLAGS) $(CPPFLAGS) -c $< -o $@

$(VIEWPORTER_OBJECT): $(VIEWPORTER_CODE) $(VIEWPORTER_HEADER) | $(OBJDIR)
	$(CC) $(CFLAGS) $(CPPFLAGS) -c $< -o $@

$(OBJDIR)/%.o: src/%.c | $(OBJDIR)
	$(CC) $(CFLAGS) $(CPPFLAGS) -MMD -MP -c $< -o $@

$(OBJDIR)/main.o: $(LAYER_HEADER) $(FRACTIONAL_HEADER) $(VIEWPORTER_HEADER) | $(OBJDIR)
	$(CC) $(CFLAGS) $(CPPFLAGS) -MMD -MP -c src/main.c -o $@

$(TARGET): $(MAIN_OBJ) $(CORE_OBJ) $(PROTOCOL_OBJECTS)
	$(CC) $(CFLAGS) $(LDFLAGS) $^ $(LDLIBS) -o $@

$(POWER_TARGET): $(POWER_OBJ)
	$(CC) $(CFLAGS) $(LDFLAGS) $^ -o $@

$(OBJDIR)/tests/unit.o: tests/unit/unit.c | $(OBJDIR)
	mkdir -p $(OBJDIR)/tests
	$(CC) $(CFLAGS) $(CPPFLAGS) -MMD -MP -c $< -o $@

$(UNIT_TARGET): $(UNIT_OBJ) $(CORE_OBJ) $(PROTOCOL_OBJECTS)
	$(CC) $(CFLAGS) $(LDFLAGS) $^ $(LDLIBS) -o $@

check test: $(UNIT_TARGET)
	$(UNIT_TARGET)

$(TEST_PROTO_DIR)/%-client-protocol.h: tests/protocols/%.xml | $(TEST_PROTO_DIR)
	$(WAYLAND_SCANNER) client-header $< $@

$(TEST_PROTO_DIR)/%-protocol.c: tests/protocols/%.xml | $(TEST_PROTO_DIR)
	$(WAYLAND_SCANNER) private-code $< $@

$(TEST_TOOL): tests/wl-input.c $(TEST_PROTOCOLS) | $(BUILD)
	$(CC) $(CFLAGS) $(CPPFLAGS) -I$(TEST_PROTO_DIR) \
		tests/wl-input.c \
		$(TEST_PROTO_DIR)/virtual-keyboard-unstable-v1-protocol.c \
		$(TEST_PROTO_DIR)/wlr-virtual-pointer-unstable-v1-protocol.c \
		$(PKG_LIBS) -lxkbcommon -o $@

multioutput: $(TARGET) $(POWER_TARGET) $(TEST_TOOL)
	$(PYTHON) tests/multioutput.py \
		--binary $(abspath $(TARGET)) \
		--power $(abspath $(POWER_TARGET)) \
		--wl-input $(abspath $(TEST_TOOL))

integration: $(TARGET) $(POWER_TARGET) $(TEST_TOOL)
	$(PYTHON) tests/integration.py \
		--binary $(abspath $(TARGET)) \
		--power $(abspath $(POWER_TARGET)) \
		--wl-input $(abspath $(TEST_TOOL))
	$(PYTHON) tests/multioutput.py \
		--binary $(abspath $(TARGET)) \
		--power $(abspath $(POWER_TARGET)) \
		--wl-input $(abspath $(TEST_TOOL))

install: all
	$(INSTALL) -d "$(DESTDIR)$(BINDIR)" "$(DESTDIR)$(LIBEXECDIR)" \
		"$(DESTDIR)$(SHAREDIR)/rx-greeter/matugen" \
		"$(DESTDIR)$(SHAREDIR)/doc/rx-greeter"
	$(INSTALL) -m755 $(TARGET) "$(DESTDIR)$(BINDIR)/rx-greeter"
	$(INSTALL) -m755 scripts/rx-greeter-session \
		"$(DESTDIR)$(BINDIR)/rx-greeter-session"
	$(INSTALL) -m755 $(POWER_TARGET) \
		"$(DESTDIR)$(LIBEXECDIR)/rx-greeter-power"
	$(INSTALL) -m755 scripts/sync-greeter-assets \
		"$(DESTDIR)$(LIBEXECDIR)/rx-greeter-sync-assets"
	$(INSTALL) -m644 configs/theme.conf \
		"$(DESTDIR)$(SHAREDIR)/rx-greeter/theme.conf"
	$(INSTALL) -m644 configs/config.conf \
		"$(DESTDIR)$(SHAREDIR)/rx-greeter/config.conf"
	$(INSTALL) -m644 configs/greetd-config.toml \
		"$(DESTDIR)$(SHAREDIR)/rx-greeter/greetd-config.toml"
	$(INSTALL) -m644 configs/matugen/rx-greeter.conf \
		"$(DESTDIR)$(SHAREDIR)/rx-greeter/matugen/rx-greeter.conf"
	$(INSTALL) -m755 scripts/install-user-theme.sh \
		"$(DESTDIR)$(SHAREDIR)/rx-greeter/install-user-theme.sh"
	$(INSTALL) -m644 README.md CHANGELOG.md LICENSE SECURITY.md \
		"$(DESTDIR)$(SHAREDIR)/doc/rx-greeter/"

install-power-setuid: install
	@if [ -n "$(DESTDIR)" ]; then \
		echo "DESTDIR is set; refusing to chown/chmod a staged helper" >&2; \
		exit 1; \
	fi
	chown root:$(POWER_GROUP) "$(LIBEXECDIR)/rx-greeter-power"
	chmod 4750 "$(LIBEXECDIR)/rx-greeter-power"
	@echo "Installed setuid power helper: $(LIBEXECDIR)/rx-greeter-power"

install-system-config:
	$(INSTALL) -d "$(DESTDIR)$(SYSCONFDIR)/rx-greeter"
	@if [ -e "$(DESTDIR)$(SYSCONFDIR)/rx-greeter/theme.conf" ]; then \
		echo "Keeping existing $(DESTDIR)$(SYSCONFDIR)/rx-greeter/theme.conf"; \
	else \
		$(INSTALL) -m644 configs/theme.conf \
			"$(DESTDIR)$(SYSCONFDIR)/rx-greeter/theme.conf"; \
	fi
	@if [ -e "$(DESTDIR)$(SYSCONFDIR)/rx-greeter/config.conf" ]; then \
		echo "Keeping existing $(DESTDIR)$(SYSCONFDIR)/rx-greeter/config.conf"; \
	else \
		$(INSTALL) -m644 configs/config.conf \
			"$(DESTDIR)$(SYSCONFDIR)/rx-greeter/config.conf"; \
	fi
	$(INSTALL) -d "$(DESTDIR)$(LOCALSTATEDIR)/rx-greeter"

uninstall:
	rm -f "$(DESTDIR)$(BINDIR)/rx-greeter" \
		"$(DESTDIR)$(BINDIR)/rx-greeter-session" \
		"$(DESTDIR)$(LIBEXECDIR)/rx-greeter-power" \
		"$(DESTDIR)$(LIBEXECDIR)/rx-greeter-sync-assets" \
		"$(DESTDIR)$(SHAREDIR)/rx-greeter/theme.conf" \
		"$(DESTDIR)$(SHAREDIR)/rx-greeter/config.conf" \
		"$(DESTDIR)$(SHAREDIR)/rx-greeter/greetd-config.toml" \
		"$(DESTDIR)$(SHAREDIR)/rx-greeter/matugen/rx-greeter.conf" \
		"$(DESTDIR)$(SHAREDIR)/rx-greeter/install-user-theme.sh" \
		"$(DESTDIR)$(SHAREDIR)/doc/rx-greeter/README.md" \
		"$(DESTDIR)$(SHAREDIR)/doc/rx-greeter/CHANGELOG.md" \
		"$(DESTDIR)$(SHAREDIR)/doc/rx-greeter/LICENSE" \
		"$(DESTDIR)$(SHAREDIR)/doc/rx-greeter/SECURITY.md"

clean:
	rm -rf $(BUILD)

-include $(shell find $(OBJDIR) -name '*.d' 2>/dev/null)
