# service.mk — encode service (0x02) linked into plh-bridge (included by src/bridge/Makefile).
# Owner: encodeur. Paths are relative to the repository root.
ENC_DIR := src/encode
SERVICE_SRCS += $(ENC_DIR)/service/enc_service.c $(ENC_DIR)/plh_enc.c $(ENC_DIR)/vaapi/va_enc.c $(ENC_DIR)/nvenc/nvenc_enc.c $(ENC_DIR)/software/sw_enc.c $(ENC_DIR)/bitstream/plh_hdr.c
SERVICE_PKGS += libva libva-drm libdrm
SERVICE_CFLAGS += -I$(ROOT)/src/encode/include -I$(ROOT)/src/bridge
