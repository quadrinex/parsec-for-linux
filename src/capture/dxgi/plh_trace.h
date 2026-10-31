#ifndef PLH_TRACE_H
#define PLH_TRACE_H
#define COBJMACROS
#include <d3d11_3.h>
/* Trace mode (PLH_D3D_TRACE=1): logs, through the process's DXVK vtables, the public D3D11 calls
 * of the application. No effect if the variable is absent. Idempotent. */
void plh_trace_install(ID3D11Device *dev, ID3D11DeviceContext *ctx, ID3D11Texture2D *tex);
void plh_trace_msg(const char *fmt, ...);
int  plh_trace_enabled(void);
#endif
