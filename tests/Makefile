# Test harness. `make test` at the root calls `make -C tests` (default target: test).
#   make -C tests            build, then run all tests (TAP output)
#   make -C tests build      build only
#   make -C tests test-full  everything, intrusive tests included; fails if any SKIP remains
#   make -C tests T="t02-pe-toolchain.sh"   run a subset
#   PLH_TEST_INTRUSIVE=1 ... allow test pattern, input injection, sound (real session)
#   V=1 ...                  show the detail of each test
REPO   := $(abspath ..)
OUT    := $(REPO)/build/tests
CC     ?= cc
PECC   ?= x86_64-w64-mingw32-gcc
CFLAGS := -std=c11 -O2 -Wall -Wextra
PEFLAGS:= -std=c11 -O2 -Wall -Wextra -static -municode -DUNICODE -D_UNICODE

NATIVE := basic xport_srv
PE     := hello dda_capture sendinput_probe wasapi_loopback clipboard_probe xport_probe
PELIBS_dda_capture      := -ld3d11 -ldxgi -luuid
PELIBS_sendinput_probe  := -luser32
PELIBS_wasapi_loopback  := -lole32 -luuid -lksuser
PELIBS_clipboard_probe  := -luser32
PELIBS_xport_probe      := -lws2_32

.PHONY: test test-full build clean
test: build
	@./run.sh $(T)

# Final criterion: everything, intrusive tests included, and zero SKIP.
test-full: build
	@PLH_TEST_INTRUSIVE=1 STRICT=1 ./run.sh $(T)

build: $(NATIVE:%=$(OUT)/native/%) $(if $(shell command -v $(PECC)),$(PE:%=$(OUT)/pe/%.exe),pe-missing)

pe-missing:
	@echo "[tests] $(PECC) missing: PE programs not built (PE tests SKIP)"

$(OUT)/native/%: native/%.c
	@mkdir -p $(@D)
	$(CC) $(CFLAGS) -o $@ $<

$(OUT)/pe/%.exe: pe/%.c pe/plh_test.h
	@mkdir -p $(@D)
	$(PECC) $(PEFLAGS) -o $@ $< $(PELIBS_$*)

clean:
	rm -rf $(OUT)
