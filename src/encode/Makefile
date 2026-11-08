# src/encode — native encoder (libplhenc.a) + encbench bench.
ROOT    := $(abspath ../..)
OUT     := $(ROOT)/build/encode
CC      ?= cc
PKGS    := libva libva-drm libdrm gbm
CFLAGS  += -std=c11 -O2 -g -Wall -Wextra -fPIC -Iinclude $(shell pkg-config --cflags $(PKGS))
LDLIBS  += $(shell pkg-config --libs $(PKGS)) -lm -ldl

LIBSRC  := plh_enc.c vaapi/va_enc.c nvenc/nvenc_enc.c software/sw_enc.c bitstream/plh_hdr.c
LIBOBJ  := $(patsubst %.c,$(OUT)/obj/%.o,$(LIBSRC))

.PHONY: all test clean
MINGW   ?= x86_64-w64-mingw32-gcc
PECFLAGS := -O2 -std=c11 -Wall -Wextra
CLIENT  := ../bridge/client/plh_client.c
PROTO_H := $(wildcard ../bridge/proto/*.h) proto/plh_proto_encode.h

all: $(OUT)/libplhenc.a $(OUT)/encbench $(OUT)/test_api $(OUT)/plh-enctest $(OUT)/plh-enctest.exe

$(OUT)/plh-enctest: tests/plh-enctest.c $(CLIENT) $(PROTO_H)
	$(CC) -std=c11 -O2 -Wall -Wextra $< $(CLIENT) -lpthread -o $@

$(OUT)/plh-enctest.exe: tests/plh-enctest.c $(CLIENT) $(PROTO_H)
	$(MINGW) $(PECFLAGS) $< $(CLIENT) -lws2_32 -o $@

$(OUT)/obj/%.o: %.c include/plh_enc.h plh_enc_internal.h bitstream/plh_hdr.h
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c $< -o $@

$(OUT)/libplhenc.a: $(LIBOBJ)
	ar rcs $@ $^

$(OUT)/encbench: bench/encbench.c $(OUT)/libplhenc.a
	$(CC) $(CFLAGS) $< $(OUT)/libplhenc.a $(LDLIBS) -o $@

$(OUT)/test_api: tests/test_api.c $(OUT)/libplhenc.a
	$(CC) $(CFLAGS) $< $(OUT)/libplhenc.a $(LDLIBS) -o $@

test: all
	$(OUT)/test_api
	./tests/run-tests.sh $(OUT)
	./tests/run-size-tests.sh native

clean:
	rm -rf $(OUT)

# ---- N3: amfrt64.dll (public AMF API, PE) + SimpleEncoder test client (mingw g++) ----
MINGWXX ?= x86_64-w64-mingw32-g++
AMFFLAGS := -std=gnu++17 -O2 -Wall -Wextra -Wno-unused-parameter -Wno-overloaded-virtual -Wno-delete-non-virtual-dtor -D_Static_assert=static_assert
$(OUT)/amf/client.o: ../bridge/client/plh_client.c $(PROTO_H)
	@mkdir -p $(dir $@)
	$(MINGW) -std=c11 -O2 -Wall -c $< -o $@

$(OUT)/amf/amfrt64.dll: amf/amfrt.cpp $(OUT)/amf/client.o $(PROTO_H)
	$(MINGWXX) $(AMFFLAGS) -shared -static -I. $< $(OUT)/amf/client.o -o $@ -lws2_32

$(OUT)/amf/amf-simple.exe: tests/amf-simple.cpp
	@mkdir -p $(dir $@)
	$(MINGWXX) $(AMFFLAGS) -static $< -o $@ -ld3d11

amf: $(OUT)/amf/amfrt64.dll $(OUT)/amf/amf-simple.exe
.PHONY: amf

$(OUT)/amf/amf-sizes.exe: tests/amf-sizes.cpp
	@mkdir -p $(dir $@)
	$(MINGWXX) $(AMFFLAGS) -static $< -o $@ -ld3d11
amf: $(OUT)/amf/amf-sizes.exe

# Size changes through amfrt64.dll under Wine, private bridge (about 6 min): make -C src/encode test-sizes
test-sizes: all amf
	./tests/run-size-tests.sh amf
.PHONY: test-sizes
