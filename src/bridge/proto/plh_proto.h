/*
 * plh_proto.h — parsec-for-linux bridge protocol (PE under Wine <-> native plh-bridge daemon)
 *
 * Owner: capture. Any change is announced to the whole team.
 * Compiles as is as C11 with native gcc/clang AND mingw-w64 (same sizes, little-endian).
 *
 * TRANSPORT (proven by src/bridge/tests/, see src/bridge/README.md)
 *  - PE clients (Wine): TCP 127.0.0.1:<port>, TCP_NODELAY. AF_UNIX winsock is NOT supported
 *    by Wine 11.20 (WSAEAFNOSUPPORT). The port and token are read from
 *    $XDG_RUNTIME_DIR/plh-bridge/{port,token} (via Z:\...). The token (32 bytes) is sent
 *    in PLH_MSG_HELLO; without a valid token the connection is closed.
 *  - Native clients: AF_UNIX SOCK_STREAM $XDG_RUNTIME_DIR/plh-bridge/bridge.sock; can
 *    receive fds (SCM_RIGHTS, e.g. DMA-BUF); authenticated by SO_PEERCRED (same uid),
 *    the token is then optional.
 *  - Bulk data (pixels, cursor, audio): files /dev/shm/plh-<uid>-<...> (0600)
 *    created by the daemon; the PE opens them via CreateFile("Z:\\dev\\shm\\...") +
 *    CreateFileMapping + MapViewOfFile (MAP_SHARED coherence proven).
 *
 * FRAMING: struct plh_msg_hdr (16 bytes) followed by hdr.size bytes of payload.
 *  - A request carries seq != 0; the reply has the same seq and the PLH_F_REPLY flag.
 *  - An unsolicited event (daemon -> client) has seq == 0 and PLH_F_EVENT.
 *  - An error reply: type = PLH_MSG_ERROR, PLH_F_REPLY, same seq.
 *  - Unknown fields: a payload may grow (appended at the end) within the same major
 *    version; the receiver ignores the excess and zero-fills what is missing.
 *
 * TYPE RANGES (high byte = service):
 *  0x00 core (capture)  0x01 capture (capture)  0x02 encoding (encode)
 *  0x03 input (input)  0x04 audio (scout)  0x05 clipboard (scout)
 *  Each owner defines its messages in proto/plh_proto_<service>.h, included below,
 *  and announces them to everyone.
 */
#ifndef PLH_PROTO_H
#define PLH_PROTO_H

#include <stdint.h>

#define PLH_PROTO_MAGIC         0x42484c50u /* "PLHB" */
#define PLH_PROTO_VERSION_MAJOR 1
#define PLH_PROTO_VERSION_MINOR 0

#define PLH_MAX_PAYLOAD   (1u << 20)   /* 1 MiB; beyond that, go through /dev/shm */
#define PLH_TOKEN_SIZE    32
#define PLH_NAME_MAX      64           /* short names (outputs, shm), NUL-terminated */

#define PLH_RUNTIME_SUBDIR "plh-bridge"           /* under $XDG_RUNTIME_DIR */
#define PLH_SOCKET_NAME    "bridge.sock"
#define PLH_PORT_FILE      "port"                 /* ASCII text : "47123\n" */
#define PLH_TOKEN_FILE     "token"                /* 32 binary bytes */

#if defined(__GNUC__) || defined(__clang__)
#define PLH_STATIC_ASSERT(c, m) _Static_assert(c, m)
#else
#define PLH_STATIC_ASSERT(c, m)
#endif

/* ---- header ---- */
struct plh_msg_hdr {
    uint32_t magic;   /* PLH_PROTO_MAGIC */
    uint16_t type;    /* enum plh_msg_type */
    uint16_t flags;   /* PLH_F_* */
    uint32_t size;    /* payload bytes after the header */
    uint32_t seq;     /* 0 for an event */
};
PLH_STATIC_ASSERT(sizeof(struct plh_msg_hdr) == 16, "hdr");

#define PLH_F_REPLY 0x0001
#define PLH_F_EVENT 0x0002
#define PLH_F_HAS_FDS 0x0004 /* native only: fds attached via SCM_RIGHTS to the 1st byte */

#define PLH_SERVICE(type) ((uint16_t)(type) >> 8)
enum plh_service_id {
    PLH_SVC_CORE = 0x00, PLH_SVC_CAPTURE = 0x01, PLH_SVC_ENCODE = 0x02,
    PLH_SVC_INPUT = 0x03, PLH_SVC_AUDIO = 0x04, PLH_SVC_CLIPBOARD = 0x05,
    PLH_SVC_COUNT
};

enum plh_status {
    PLH_OK = 0,
    PLH_E_PROTO = -1,      /* malformed message */
    PLH_E_VERSION = -2,    /* incompatible major version */
    PLH_E_AUTH = -3,       /* invalid token */
    PLH_E_UNKNOWN = -4,    /* unknown message type / service absent */
    PLH_E_INVAL = -5,      /* invalid argument */
    PLH_E_NOENT = -6,      /* nonexistent object (output, stream...) */
    PLH_E_BUSY = -7,
    PLH_E_NOMEM = -8,
    PLH_E_IO = -9,
    PLH_E_PERM = -10,      /* permission (portal) denied or pending */
    PLH_E_AGAIN = -11,     /* not ready yet (e.g. human approval pending) */
};

/* ---- core (0x00xx) ---- */
enum plh_msg_type_core {
    PLH_MSG_HELLO       = 0x0001, /* C->D plh_hello; reply plh_hello_reply */
    PLH_MSG_ERROR       = 0x0002, /* D->C plh_error (reply) */
    PLH_MSG_PING        = 0x0003, /* C->D any payload; reply: same payload + plh_pong in front */
    PLH_MSG_BYE         = 0x0004, /* C->D clean close, no reply */
    PLH_MSG_LOG         = 0x0005, /* C->D UTF-8 text logged by the daemon (PE debugging) */
};

enum plh_client_kind {
    PLH_CLIENT_TOOL = 0, PLH_CLIENT_DXGI = 1, PLH_CLIENT_ENCODER = 2, PLH_CLIENT_INPUT = 3,
    PLH_CLIENT_AUDIO = 4, PLH_CLIENT_CLIPBOARD = 5, PLH_CLIENT_TEST = 6,
};

struct plh_hello {
    uint16_t version_major;
    uint16_t version_minor;
    uint32_t client_kind;               /* enum plh_client_kind */
    uint32_t pid;                       /* Windows or native pid, for logs */
    uint32_t reserved;
    uint8_t  token[PLH_TOKEN_SIZE];     /* mandatory over TCP */
    char     name[PLH_NAME_MAX];        /* ex. "parsecd.exe/dxgi" */
};
PLH_STATIC_ASSERT(sizeof(struct plh_hello) == 112, "hello");

struct plh_hello_reply {
    int32_t  status;                    /* enum plh_status */
    uint16_t version_major;
    uint16_t version_minor;
    uint32_t services;                  /* bit (1<<svc) for each available service */
    uint32_t client_id;                 /* unique id on the daemon side */
    uint64_t daemon_time_ns;            /* daemon CLOCK_MONOTONIC, to correlate pts */
    uint32_t uid;                       /* Unix uid of the daemon (/dev/shm names) */
    uint32_t reserved;
};
PLH_STATIC_ASSERT(sizeof(struct plh_hello_reply) == 32, "hello_reply");

struct plh_error {
    int32_t  status;                    /* enum plh_status */
    uint16_t req_type;
    uint16_t reserved;
    char     message[128];
};

struct plh_pong {
    uint64_t daemon_time_ns;            /* CLOCK_MONOTONIC */
};

/* ---- pixel formats (shared by capture/encode) ---- */
enum plh_pixel_format {
    PLH_FMT_INVALID = 0,
    PLH_FMT_BGRA8 = 1,   /* = DXGI_FORMAT_B8G8R8A8_UNORM, bytes B,G,R,A; = DRM_FORMAT_ARGB8888 */
    PLH_FMT_BGRX8 = 2,   /* alpha ignored; = DRM_FORMAT_XRGB8888 */
    PLH_FMT_RGBA8 = 3,   /* = DRM_FORMAT_ABGR8888 */
    PLH_FMT_RGBX8 = 4,   /* = DRM_FORMAT_XBGR8888 */
    PLH_FMT_NV12  = 5,
};

/* Global id of a captured frame: (stream_id << 40) | frame number.
 * Valid while the frame is held (between FRAME and FRAME_RELEASE). The encoder
 * (service 0x02, in the same daemon) can resolve it to the original DMA-BUF (not implemented yet). */
#define PLH_FRAME_ID(stream, n) (((uint64_t)(stream) << 40) | ((uint64_t)(n) & 0xffffffffffull))
#define PLH_FRAME_STREAM(id)    ((uint32_t)((id) >> 40))

#include "plh_proto_capture.h"
#include "plh_proto_input.h"
#include "../../encode/proto/plh_proto_encode.h"

#endif /* PLH_PROTO_H */
