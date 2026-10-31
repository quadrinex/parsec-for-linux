# service.mk — "input" service linked into plh-bridge (included by src/bridge/Makefile).
# Paths are relative to the repository root.
IN_DIR := src/input
SERVICE_SRCS += $(IN_DIR)/bridge_input.c $(IN_DIR)/input.c $(IN_DIR)/scancode.c $(IN_DIR)/uinput.c \
                $(IN_DIR)/kwin_dbus.c $(IN_DIR)/layout.c
SERVICE_CFLAGS += -I$(ROOT)/src/input/include -I$(ROOT)/src/bridge
SERVICE_PKGS += libsystemd
