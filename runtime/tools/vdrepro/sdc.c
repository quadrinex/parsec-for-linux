/* Mimics Parsec: QueryDisplayConfig(ACTIVE_PATHS) then SetDisplayConfig re-applying it (flags 0x4a0). Prints counts + result. */
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>

int main(void)
{
    UINT32 np = 0, nm = 0;
    DISPLAYCONFIG_PATH_INFO *p; DISPLAYCONFIG_MODE_INFO *m;
    LONG r = GetDisplayConfigBufferSizes(QDC_ONLY_ACTIVE_PATHS, &np, &nm);
    if (r) { printf("GetDisplayConfigBufferSizes %ld\n", r); return 1; }
    p = calloc(np, sizeof(*p)); m = calloc(nm, sizeof(*m));
    r = QueryDisplayConfig(QDC_ONLY_ACTIVE_PATHS, &np, p, &nm, m, NULL);
    printf("QueryDisplayConfig: %ld paths %u modes %u\n", r, np, nm);
    if (r) return 1;
    r = SetDisplayConfig(np, p, nm, m, SDC_APPLY | SDC_USE_SUPPLIED_DISPLAY_CONFIG | SDC_ALLOW_CHANGES);
    printf("SetDisplayConfig: %ld\n", r);
    return r != 0;
}
