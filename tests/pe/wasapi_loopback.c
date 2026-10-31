/* T5 — WASAPI loopback audio capture of the default output,
 * as Parsec does to send the sound to the client (to be confirmed by api-map).
 *   wasapi_loopback.exe [secondes]
 * Prints the mix format, the number of frames received, the peak and the RMS
 * (full scale = 1.0). Plays no sound. */
#define COBJMACROS
#define INITGUID
#include "plh_test.h"
#include <initguid.h>
#include <mmdeviceapi.h>
#include <audioclient.h>
#include <math.h>
#include <stdlib.h>

DEFINE_GUID(PLH_KSDATAFORMAT_SUBTYPE_IEEE_FLOAT, 0x00000003, 0x0000, 0x0010, 0x80, 0x00, 0x00, 0xaa, 0x00, 0x38, 0x9b, 0x71);

int wmain(int argc, wchar_t **argv) {
    double secs = argc > 1 ? _wtof(argv[1]) : 3.0;
    HRESULT hr = CoInitializeEx(NULL, COINIT_MULTITHREADED);
    IMMDeviceEnumerator *en = NULL; IMMDevice *dev = NULL; IAudioClient *ac = NULL; IAudioCaptureClient *cc = NULL;
    hr = CoCreateInstance(&CLSID_MMDeviceEnumerator, NULL, CLSCTX_ALL, &IID_IMMDeviceEnumerator, (void **)&en);
    OK(SUCCEEDED(hr), "CoCreateInstance(MMDeviceEnumerator) hr=0x%08lx", hr); if (FAILED(hr)) return 1;
    hr = IMMDeviceEnumerator_GetDefaultAudioEndpoint(en, eRender, eConsole, &dev);
    OK(SUCCEEDED(hr), "GetDefaultAudioEndpoint(eRender) hr=0x%08lx", hr); if (FAILED(hr)) return 1;
    LPWSTR id = NULL; IMMDevice_GetId(dev, &id); plh_out("device_id=%ls\n", id ? id : L"?");
    hr = IMMDevice_Activate(dev, &IID_IAudioClient, CLSCTX_ALL, NULL, (void **)&ac);
    OK(SUCCEEDED(hr), "Activate(IAudioClient) hr=0x%08lx", hr); if (FAILED(hr)) return 1;
    WAVEFORMATEX *wf = NULL; IAudioClient_GetMixFormat(ac, &wf);
    int isfloat = wf->wFormatTag == WAVE_FORMAT_IEEE_FLOAT ||
        (wf->wFormatTag == WAVE_FORMAT_EXTENSIBLE && IsEqualGUID(&((WAVEFORMATEXTENSIBLE *)wf)->SubFormat, &PLH_KSDATAFORMAT_SUBTYPE_IEEE_FLOAT));
    plh_out("mix_format rate=%lu channels=%u bits=%u float=%d\n", wf->nSamplesPerSec, wf->nChannels, wf->wBitsPerSample, isfloat);
    hr = IAudioClient_Initialize(ac, AUDCLNT_SHAREMODE_SHARED, AUDCLNT_STREAMFLAGS_LOOPBACK, 200000, 0, wf, NULL);
    OK(SUCCEEDED(hr), "Initialize(SHARED, LOOPBACK) hr=0x%08lx", hr); if (FAILED(hr)) return 1;
    hr = IAudioClient_GetService(ac, &IID_IAudioCaptureClient, (void **)&cc);
    OK(SUCCEEDED(hr), "GetService(IAudioCaptureClient) hr=0x%08lx", hr); if (FAILED(hr)) return 1;
    IAudioClient_Start(ac);
    double t0 = plh_now(), peak = 0, sumsq = 0; unsigned long long frames = 0, silent_pkts = 0, pkts = 0;
    while (plh_now() - t0 < secs) {
        Sleep(10);
        UINT32 n = 0;
        while (SUCCEEDED(IAudioCaptureClient_GetNextPacketSize(cc, &n)) && n) {
            BYTE *data; UINT32 nf; DWORD fl;
            if (FAILED(IAudioCaptureClient_GetBuffer(cc, &data, &nf, &fl, NULL, NULL))) break;
            pkts++;
            if (fl & AUDCLNT_BUFFERFLAGS_SILENT) silent_pkts++;
            else for (UINT32 i = 0; i < nf * wf->nChannels; i++) {
                double v = isfloat && wf->wBitsPerSample == 32 ? ((float *)data)[i]
                         : wf->wBitsPerSample == 16 ? ((short *)data)[i] / 32768.0
                         : wf->wBitsPerSample == 32 ? ((int *)data)[i] / 2147483648.0 : 0;
                if (fabs(v) > peak) peak = fabs(v);
                sumsq += v * v;
            }
            frames += nf;
            IAudioCaptureClient_ReleaseBuffer(cc, nf);
        }
    }
    IAudioClient_Stop(ac);
    double rms = frames ? sqrt(sumsq / (double)(frames * wf->nChannels)) : 0;
    plh_out("captured frames=%llu packets=%llu silent_packets=%llu secs=%.2f peak=%.4f rms=%.5f\n",
            frames, pkts, silent_pkts, plh_now() - t0, peak, rms);
    OK(frames > 0, "frames were received");
    return plh_failures ? 1 : 0;
}
