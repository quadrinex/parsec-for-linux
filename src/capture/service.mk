# service.mk — sources of the capture service linked into plh-bridge (included by src/bridge/Makefile).
# Owner: capture. Variables relative to the repository root.
CAP_DIR := src/capture
SERVICE_SRCS += $(CAP_DIR)/cap_service.c $(CAP_DIR)/outputs_wl.c $(CAP_DIR)/backend_test.c $(CAP_DIR)/backend_stubs.c \
                $(CAP_DIR)/pw_consumer.c $(CAP_DIR)/backend_kwin.c \
                $(CAP_DIR)/backend_portal.c $(CAP_DIR)/portal_token.c $(CAP_DIR)/backend_x11.c $(CAP_DIR)/cap_detect.c $(CAP_DIR)/vout.c
SERVICE_PKGS += wayland-client libpipewire-0.3 libsystemd xcb xcb-shm xcb-randr xcb-xfixes
SERVICE_WL_PROTOCOLS += $(CAP_DIR)/protocols/xdg-output-unstable-v1.xml $(CAP_DIR)/protocols/zkde-screencast-unstable-v1.xml
SERVICE_CFLAGS += -DPLH_HAVE_KWIN -DPLH_HAVE_PORTAL
SERVICE_LIBS += -lm
