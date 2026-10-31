#define COBJMACROS
#include <initguid.h>
#include <windows.h>
#include <mfapi.h>
#include <mftransform.h>
#include <mfobjects.h>
#include <mferror.h>
#include <codecapi.h>
#include <stdio.h>
DEFINE_GUID(CLSID_H264Enc,0x6ca50344,0x051a,0x4ded,0x97,0x79,0xa4,0x33,0x05,0x16,0x5e,0x35);
#define W 1920
#define H 1080
#define CK(x) do{HRESULT h_=(x);if(FAILED(h_)){printf("FAIL %s hr=%08lx\n",#x,(unsigned long)h_);return 1;}}while(0)
int main(int argc,char**argv){
 const char*out=argc>1?argv[1]:"Z:\\tmp\\plh-eclaireur\\mft\\out.h264";
 CoInitializeEx(0,COINIT_MULTITHREADED); CK(MFStartup(MF_VERSION,0));
 IMFTransform*t; CK(CoCreateInstance(&CLSID_H264Enc,0,CLSCTX_INPROC_SERVER,&IID_IMFTransform,(void**)&t));
 IMFMediaType*ot,*it; CK(MFCreateMediaType(&ot));
 IMFMediaType_SetGUID(ot,&MF_MT_MAJOR_TYPE,&MFMediaType_Video); IMFMediaType_SetGUID(ot,&MF_MT_SUBTYPE,&MFVideoFormat_H264);
 IMFMediaType_SetUINT32(ot,&MF_MT_AVG_BITRATE,8000000); IMFMediaType_SetUINT64(ot,&MF_MT_FRAME_SIZE,((UINT64)W<<32)|H);
 IMFMediaType_SetUINT64(ot,&MF_MT_FRAME_RATE,((UINT64)30<<32)|1); IMFMediaType_SetUINT32(ot,&MF_MT_INTERLACE_MODE,2);
 IMFMediaType_SetUINT64(ot,&MF_MT_PIXEL_ASPECT_RATIO,((UINT64)1<<32)|1); IMFMediaType_SetUINT32(ot,&MF_MT_MPEG2_PROFILE,66);
 CK(IMFTransform_SetOutputType(t,0,ot,0));
 CK(MFCreateMediaType(&it));
 IMFMediaType_SetGUID(it,&MF_MT_MAJOR_TYPE,&MFMediaType_Video); IMFMediaType_SetGUID(it,&MF_MT_SUBTYPE,&MFVideoFormat_NV12);
 IMFMediaType_SetUINT64(it,&MF_MT_FRAME_SIZE,((UINT64)W<<32)|H); IMFMediaType_SetUINT64(it,&MF_MT_FRAME_RATE,((UINT64)30<<32)|1);
 IMFMediaType_SetUINT32(it,&MF_MT_INTERLACE_MODE,2); IMFMediaType_SetUINT64(it,&MF_MT_PIXEL_ASPECT_RATIO,((UINT64)1<<32)|1);
 CK(IMFTransform_SetInputType(t,0,it,0));
 MFT_OUTPUT_STREAM_INFO si; CK(IMFTransform_GetOutputStreamInfo(t,0,&si));
 printf("outinfo flags=%lx cb=%lu align=%lu\n",(unsigned long)si.dwFlags,(unsigned long)si.cbSize,(unsigned long)si.cbAlignment);
 CK(IMFTransform_ProcessMessage(t,MFT_MESSAGE_NOTIFY_BEGIN_STREAMING,0));
 HANDLE f=CreateFileA(out,GENERIC_WRITE,0,0,CREATE_ALWAYS,0,0); DWORD total=0;
 UINT32 sz=W*H*3/2;
 for(int i=0;i<40;i++){
  IMFMediaBuffer*b; CK(MFCreateMemoryBuffer(sz,&b)); BYTE*p; CK(IMFMediaBuffer_Lock(b,&p,0,0));
  memset(p,81,W*H); for(int k=0;k<W*H/2;k+=2){p[W*H+k]=90;p[W*H+k+1]=240;} /* red */
  IMFMediaBuffer_Unlock(b); IMFMediaBuffer_SetCurrentLength(b,sz);
  IMFSample*s; CK(MFCreateSample(&s)); IMFSample_AddBuffer(s,b); IMFSample_SetSampleTime(s,(LONGLONG)i*333333); IMFSample_SetSampleDuration(s,333333);
  HRESULT hr=IMFTransform_ProcessInput(t,0,s,0); if(FAILED(hr)){printf("ProcessInput %d hr=%08lx\n",i,(unsigned long)hr);return 1;}
  for(;;){
   MFT_OUTPUT_DATA_BUFFER ob={0}; DWORD st=0; IMFSample*os=0;
   if(!(si.dwFlags&(MFT_OUTPUT_STREAM_PROVIDES_SAMPLES|MFT_OUTPUT_STREAM_CAN_PROVIDE_SAMPLES))){
     IMFMediaBuffer*o; MFCreateMemoryBuffer(si.cbSize?si.cbSize:1<<20,&o); MFCreateSample(&os); IMFSample_AddBuffer(os,o); ob.pSample=os; }
   hr=IMFTransform_ProcessOutput(t,0,1,&ob,&st);
   if(hr==MF_E_TRANSFORM_NEED_MORE_INPUT)break; if(FAILED(hr)){printf("ProcessOutput hr=%08lx\n",(unsigned long)hr);return 1;}
   IMFMediaBuffer*ob2; IMFSample_ConvertToContiguousBuffer(ob.pSample,&ob2); BYTE*d; DWORD n; IMFMediaBuffer_Lock(ob2,&d,0,&n);
   DWORD w; WriteFile(f,d,n,&w,0); total+=n; IMFMediaBuffer_Unlock(ob2);
  }
 }
 CloseHandle(f); printf("encoded bytes=%lu\n",(unsigned long)total); return 0;}
