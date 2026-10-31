/* backend_stubs.c — stubs for backends compiled out. Owner: capture. */
#include "cap_internal.h"

#ifndef PLH_HAVE_KWIN
static int kwin_start(struct cap_stream *s, const struct plh_cap_output *o) { (void)s; (void)o; return PLH_E_UNKNOWN; }
static void kwin_stop(struct cap_stream *s) { (void)s; }
const struct cap_backend_ops cap_backend_kwin = { "kwin", PLH_CAP_BACKEND_KWIN, kwin_start, kwin_stop };
#endif
