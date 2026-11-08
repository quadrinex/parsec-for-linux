/* plh-bridge-inject — test tool: connects to a plh-bridge as an INPUT client and sends Windows-space
 * input events through service 0x03 (what plh-input-hook does). Commands on stdin:
 *   abs WX WY | click | sleep MS
 * Uses PLH_BRIDGE_DIR like every bridge client. Real injection: only under the input-safety rules. */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "../../bridge/client/plh_client.h"

static int send_ev(struct plh_conn *c, int type, int code, int a, int b)
{
    struct { struct plh_input_events_hdr h; struct plh_input_ev e; } m;
    memset(&m, 0, sizeof m);
    m.h.count = 1; m.e.type = (uint8_t)type; m.e.code = (uint16_t)code; m.e.a = a; m.e.b = b;
    struct plh_input_status st; uint32_t n = 0;
    int r = plh_conn_request(c, PLH_MSG_INPUT_EVENTS, &m, sizeof m, &st, sizeof st, &n, 2000);
    return r < 0 ? r : (int)st.applied;   /* 1 = applied, 0 = dropped/ignored */
}

int main(void)
{
    struct plh_conn *c;
    if (plh_conn_open(&c, PLH_CLIENT_INPUT, "plh-bridge-inject", NULL) != PLH_OK) { fprintf(stderr, "cannot connect\n"); return 1; }
    char line[256];
    while (fgets(line, sizeof line, stdin)) {
        int x, y, ms;
        if (sscanf(line, "abs %d %d", &x, &y) == 2) printf("abs %d %d -> applied=%d\n", x, y, send_ev(c, PLH_IEV_MOVE_ABS, 0, x, y));
        else if (!strncmp(line, "click", 5)) { send_ev(c, PLH_IEV_BUTTON, PLH_IBTN_LEFT, 0, 1); struct timespec t = { 0, 20000000 }; nanosleep(&t, NULL); printf("click -> applied=%d\n", send_ev(c, PLH_IEV_BUTTON, PLH_IBTN_LEFT, 0, 0)); }
        else if (sscanf(line, "sleep %d", &ms) == 1) { struct timespec t = { ms / 1000, (ms % 1000) * 1000000L }; nanosleep(&t, NULL); }
        fflush(stdout);
    }
    plh_conn_close(c);
    return 0;
}
