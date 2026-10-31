# Root Makefile: delegates to each module (each has its own local Makefile).
MODULES := src/bridge src/capture src/encode src/input src/audio src/clipboard

.PHONY: all amf test test-full clean $(MODULES)

all: $(MODULES) amf

amf: src/encode
	$(MAKE) -C src/encode amf

$(MODULES):
	@if [ -f $@/Makefile ]; then $(MAKE) -C $@; else echo "[skip] $@ : no Makefile"; fi

test: all
	@if [ -f tests/Makefile ]; then $(MAKE) -C tests; else echo "tests/Makefile absent"; exit 1; fi

# Criterion 5: everything, intrusive tests included, fails on any SKIP.
test-full: all
	$(MAKE) -C tests test-full

clean:
	@for m in $(MODULES) tests; do [ -f $$m/Makefile ] && $(MAKE) -C $$m clean || true; done
	rm -rf build
