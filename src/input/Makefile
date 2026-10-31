# src/input — input injection backends (native Linux).
B      ?= ../../build/input
CC     ?= cc
CFLAGS ?= -O2 -g
CFLAGS += -std=c11 -Wall -Wextra -Iinclude
LIBSRC := input.c scancode.c uinput.c kwin_dbus.c layout.c
LDLIBS += -lsystemd
LIBOBJ := $(LIBSRC:%.c=$(B)/%.o)

MINGW  ?= x86_64-w64-mingw32-gcc
PECFLAGS := -O2 -std=c11 -Wall -Wextra
CLIENT := ../bridge/client/plh_client.c
PEHDRS := $(wildcard ../bridge/proto/*.h ../bridge/client/*.h)

all: $(B)/test-layout $(B)/libplhinput.a $(B)/plh-input-ctl $(B)/test-scancode $(B)/test-uinput-seq $(B)/pe/plh-input-hook.exe $(B)/pe/injectprobe.exe

$(B)/pe/plh-input-hook.exe: pe/plh-input-hook.c $(CLIENT) $(PEHDRS)
	@mkdir -p $(B)/pe
	$(MINGW) $(PECFLAGS) -o $@ pe/plh-input-hook.c $(CLIENT) -lws2_32
$(B)/pe/injectprobe.exe: pe/injectprobe.c
	@mkdir -p $(B)/pe
	$(MINGW) $(PECFLAGS) -o $@ $<

$(B)/%.o: %.c include/plh_input.h | $(B)
	$(CC) $(CFLAGS) -c $< -o $@
$(B)/tools/%.o: tools/%.c include/plh_input.h | $(B)
	@mkdir -p $(B)/tools
	$(CC) $(CFLAGS) -c $< -o $@
$(B)/libplhinput.a: $(LIBOBJ)
	ar rcs $@ $^
$(B)/plh-input-ctl: $(B)/tools/plh-input-ctl.o $(B)/libplhinput.a
	$(CC) -o $@ $^ $(LDLIBS)
$(B)/test-scancode: $(B)/tools/test-scancode.o $(B)/libplhinput.a
	$(CC) -o $@ $^ $(LDLIBS)
$(B)/test-uinput-seq: $(B)/tools/test-uinput-seq.o $(B)/libplhinput.a
	$(CC) -o $@ $^ $(LDLIBS)
$(B):
	mkdir -p $(B)

# Unit tests without injection (safe).
$(B)/test-layout: $(B)/tools/test-layout.o $(B)/libplhinput.a
	$(CC) -o $@ $^ $(LDLIBS)

test: all
	$(B)/test-scancode
	$(B)/test-uinput-seq
	$(B)/test-layout

clean:
	rm -rf $(B)
ei-check:
	$(CC) $(CFLAGS) -I/usr/include/libei-1.0 -fsyntax-only ei.c
.PHONY: all test clean ei-check
