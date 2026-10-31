/* pw_consumer.h — shared PipeWire consumer (KWin and portal backends). Owner: capture. */
#ifndef PW_CONSUMER_H
#define PW_CONSUMER_H
#include "cap_internal.h"

struct cap_pw;

/* pw_fd: remote fd from the portal (OpenPipeWireRemote) or -1 for the session PipeWire daemon.
 * target: serial number (object.serial) or node id, as text.
 * The consumer copies each frame into a slot of stream `s` and pushes the cursor shape. */
struct cap_pw *cap_pw_start(struct cap_stream *s, int pw_fd, const char *target, uint32_t node_id);
void cap_pw_stop(struct cap_pw *p);

#endif
