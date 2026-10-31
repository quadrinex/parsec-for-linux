# PE dxgi.dll proxy. Owner: capture. Output: build/dxgi/dxgi.dll
ROOT  ?= $(abspath ../../..)
B     := $(ROOT)/build/dxgi
MINGW ?= x86_64-w64-mingw32-gcc
CL    := $(ROOT)/src/bridge/client/plh_client.c
all: $(B)/dxgi.dll
$(B)/dxgi.dll: plh_dxgi.c plh_trace.c plh_trace.h dxgi.def $(CL) $(wildcard $(ROOT)/src/bridge/proto/*.h $(ROOT)/src/bridge/client/*.h)
	@mkdir -p $(B)
	$(MINGW) -std=gnu11 -O2 -Wall -Wextra -shared -static-libgcc -o $@ plh_dxgi.c plh_trace.c $(CL) dxgi.def -ldxguid -luuid -lws2_32 -lole32
clean:
	rm -rf $(B)
