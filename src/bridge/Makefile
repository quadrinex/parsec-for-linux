# src/bridge/Makefile — plh-bridge daemon + client library + tools/tests. Owner: capture.
# Called from the root: make -C src/bridge (ROOT = repository root).
ROOT  ?= $(abspath ../..)
B     := $(ROOT)/build/bridge
CC    ?= cc
MINGW ?= x86_64-w64-mingw32-gcc
CFLAGS ?= -O2 -g
CFLAGS += -std=c11 -Wall -Wextra
PECFLAGS := -O2 -std=c11 -Wall -Wextra

SERVICE_SRCS :=
SERVICE_PKGS :=
SERVICE_LIBS :=
SERVICE_CFLAGS :=
SERVICE_WL_PROTOCOLS :=
# each module may provide src/<module>/service.mk (see src/bridge/README.md)
-include $(wildcard $(ROOT)/src/*/service.mk)

SRCS := $(ROOT)/src/bridge/bridge.c $(addprefix $(ROOT)/,$(SERVICE_SRCS))
WLGEN_H := $(patsubst %.xml,$(B)/gen/%-client-protocol.h,$(notdir $(SERVICE_WL_PROTOCOLS)))
WLGEN_C := $(patsubst %.xml,$(B)/gen/%-protocol.c,$(notdir $(SERVICE_WL_PROTOCOLS)))
PKG_CFLAGS := $(if $(SERVICE_PKGS),$(shell pkg-config --cflags $(sort $(SERVICE_PKGS))))
PKG_LIBS   := $(if $(SERVICE_PKGS),$(shell pkg-config --libs $(sort $(SERVICE_PKGS))))
vpath %.xml $(sort $(dir $(addprefix $(ROOT)/,$(SERVICE_WL_PROTOCOLS))))

CLIENT := $(ROOT)/src/bridge/client/plh_client.c
HDRS := $(wildcard $(ROOT)/src/bridge/*.h $(ROOT)/src/bridge/proto/*.h $(ROOT)/src/bridge/client/*.h $(ROOT)/src/*/cap_internal.h)

all: $(B)/plh-bridge $(B)/plh-captest $(B)/plh-captest.exe $(B)/proto-sizes $(B)/proto-sizes.exe $(B)/afunix-probe.exe $(B)/vout-probe $(B)/plh-vout-test dxgi-proxy

dxgi-proxy:
	$(MAKE) -C $(ROOT)/src/capture/dxgi ROOT=$(ROOT)
.PHONY: dxgi-proxy

$(B)/gen/%-client-protocol.h: %.xml
	@mkdir -p $(dir $@)
	wayland-scanner client-header $< $@
$(B)/gen/%-protocol.c: %.xml
	@mkdir -p $(dir $@)
	wayland-scanner private-code $< $@

$(B)/plh-bridge: $(SRCS) $(WLGEN_C) $(WLGEN_H) $(HDRS)
	@mkdir -p $(B)
	$(CC) $(CFLAGS) $(SERVICE_CFLAGS) $(PKG_CFLAGS) -I$(B)/gen -o $@ $(SRCS) $(WLGEN_C) $(PKG_LIBS) $(SERVICE_LIBS)

$(B)/vout-probe: $(ROOT)/src/capture/tools/vout-probe.c $(B)/gen/zkde-screencast-unstable-v1-client-protocol.h $(B)/gen/zkde-screencast-unstable-v1-protocol.c
	@mkdir -p $(B)
	$(CC) $(CFLAGS) $(shell pkg-config --cflags wayland-client libpipewire-0.3) -I$(B)/gen -o $@ $< $(B)/gen/zkde-screencast-unstable-v1-protocol.c $(shell pkg-config --libs wayland-client libpipewire-0.3)

$(B)/plh-captest: $(ROOT)/src/capture/tools/plh-captest.c $(CLIENT) $(HDRS)
	@mkdir -p $(B)
	$(CC) $(CFLAGS) -o $@ $(ROOT)/src/capture/tools/plh-captest.c $(CLIENT) -lpthread

$(B)/plh-vout-test: $(ROOT)/src/capture/tools/plh-vout-test.c $(CLIENT) $(HDRS)
	@mkdir -p $(B)
	$(CC) $(CFLAGS) -o $@ $(ROOT)/src/capture/tools/plh-vout-test.c $(CLIENT) -lpthread

$(B)/plh-captest.exe: $(ROOT)/src/capture/tools/plh-captest.c $(CLIENT) $(HDRS)
	@mkdir -p $(B)
	$(MINGW) $(PECFLAGS) -o $@ $(ROOT)/src/capture/tools/plh-captest.c $(CLIENT) -lws2_32

$(B)/proto-sizes: $(ROOT)/src/bridge/tests/proto-sizes.c $(HDRS)
	@mkdir -p $(B)
	$(CC) $(CFLAGS) -o $@ $<
$(B)/proto-sizes.exe: $(ROOT)/src/bridge/tests/proto-sizes.c $(HDRS)
	@mkdir -p $(B)
	$(MINGW) $(PECFLAGS) -o $@ $<

$(B)/test-token: $(ROOT)/src/capture/tools/test_token.c $(ROOT)/src/capture/portal_token.c $(HDRS)
	@mkdir -p $(B)
	$(CC) $(CFLAGS) -o $@ $(ROOT)/src/capture/tools/test_token.c $(ROOT)/src/capture/portal_token.c

test: all $(B)/test-token
	$(ROOT)/src/bridge/tests/run-bridge-tests.sh $(B)

clean:
	rm -rf $(B)

.PHONY: all test clean

$(B)/afunix-probe.exe: $(ROOT)/src/bridge/tests/afunix-probe.c
	@mkdir -p $(B)
	$(MINGW) $(PECFLAGS) -o $@ $< -lws2_32
