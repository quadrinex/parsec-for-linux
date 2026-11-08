# src/input — input injection backends (native Linux).
B      ?= ../../build/input
CC     ?= cc
CFLAGS ?= -O2 -g
CFLAGS += -std=c11 -Wall -Wextra -Iinclude
LIBSRC := pad.c input.c scancode.c uinput.c kwin_dbus.c layout.c
LDLIBS += -lsystemd
LIBOBJ := $(LIBSRC:%.c=$(B)/%.o)

MINGW  ?= x86_64-w64-mingw32-gcc
PECFLAGS := -O2 -std=c11 -Wall -Wextra
CLIENT := ../bridge/client/plh_client.c
PEHDRS := $(wildcard ../bridge/proto/*.h ../bridge/client/*.h)

all: $(B)/pe/plh-pad-relay.exe $(B)/pe/plh-vigem.sys $(B)/pe/padprobe.exe $(B)/test-layout $(B)/test-vdisplay $(B)/plh-bridge-inject $(B)/pad-evdev-dump $(B)/libplhinput.a $(B)/plh-input-ctl $(B)/test-scancode $(B)/test-uinput-seq $(B)/pe/plh-input-hook.exe $(B)/pe/injectprobe.exe

$(B)/pe/plh-input-hook.exe: pe/plh-input-hook.c $(CLIENT) $(PEHDRS)
	@mkdir -p $(B)/pe
	$(MINGW) $(PECFLAGS) -o $@ pe/plh-input-hook.c $(CLIENT) -lws2_32
$(B)/pe/plh-pad-relay.exe: gamepad/plh-pad-relay.c gamepad/plh_pad_drv.h $(CLIENT) $(PEHDRS)
	@mkdir -p $(B)/pe
	$(MINGW) $(PECFLAGS) -Igamepad -o $@ gamepad/plh-pad-relay.c $(CLIENT) -lws2_32 -ladvapi32
$(B)/pe/plh-vigem.sys: gamepad/plh-vigem.c gamepad/plh_pad_drv.h
	@mkdir -p $(B)/pe
	$(MINGW) $(PECFLAGS) -I/usr/x86_64-w64-mingw32/include/ddk -Igamepad -nostartfiles -nostdlib -shared \
	  -Wl,--subsystem,native -Wl,--entry,DriverEntry -o $@ gamepad/plh-vigem.c -lntoskrnl
$(B)/pe/padprobe.exe: gamepad/padprobe.c gamepad/plh_pad_drv.h
	@mkdir -p $(B)/pe
	$(MINGW) $(PECFLAGS) -Igamepad -o $@ gamepad/padprobe.c -lsetupapi
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
$(B)/pad-evdev-dump: $(B)/tools/pad-evdev-dump.o
	$(CC) -o $@ $^
$(B):
	mkdir -p $(B)

# Unit tests without injection (safe).
$(B)/test-layout: $(B)/tools/test-layout.o $(B)/libplhinput.a
	$(CC) -o $@ $^ $(LDLIBS)

$(B)/plh-bridge-inject: tools/plh-bridge-inject.c $(CLIENT) $(PEHDRS) | $(B)
	$(CC) $(CFLAGS) -o $@ tools/plh-bridge-inject.c $(CLIENT) -lpthread
$(B)/test-vdisplay: tests/test-vdisplay.c $(B)/libplhinput.a include/plh_input.h
	$(CC) $(CFLAGS) -o $@ tests/test-vdisplay.c $(B)/libplhinput.a $(LDLIBS)

test: all
	$(B)/test-scancode
	$(B)/test-uinput-seq
	$(B)/test-layout
	$(B)/test-vdisplay

clean:
	rm -rf $(B)
ei-check:
	$(CC) $(CFLAGS) -I/usr/include/libei-1.0 -fsyntax-only ei.c
.PHONY: all test clean ei-check
