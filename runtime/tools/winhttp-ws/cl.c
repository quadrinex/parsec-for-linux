#include <windows.h>
#include <winhttp.h>
#include <stdio.h>
static HINTERNET ws; static char buf[2048]; static volatile LONG errs, reads; static HANDLE ev;
static void CALLBACK cb(HINTERNET h,DWORD_PTR c,DWORD st,void*info,DWORD len){
 if(st==WINHTTP_CALLBACK_STATUS_READ_COMPLETE){reads++;SetEvent(ev);}
 else if(st==WINHTTP_CALLBACK_STATUS_REQUEST_ERROR){WINHTTP_WEB_SOCKET_ASYNC_RESULT*r=info;
  if(errs++<3)printf("err op=%lu dwError=0x%lX\n",(unsigned long)r->Operation,(unsigned long)r->AsyncResult.dwError);SetEvent(ev);}}
int main(void){ev=CreateEventA(0,0,0,0);
 HINTERNET s=WinHttpOpen(L"t",0,0,0,WINHTTP_FLAG_ASYNC);HINTERNET c=WinHttpConnect(s,L"127.0.0.1",18765,0);
 HINTERNET r=WinHttpOpenRequest(c,L"GET",L"/",0,0,0,0);
 WinHttpSetOption(r,WINHTTP_OPTION_UPGRADE_TO_WEB_SOCKET,0,0);
 WinHttpSetStatusCallback(r,cb,WINHTTP_CALLBACK_FLAG_ALL_NOTIFICATIONS,0);
 WinHttpSendRequest(r,0,0,0,0,0,0);Sleep(500);WinHttpReceiveResponse(r,0);Sleep(500);
 ws=WinHttpWebSocketCompleteUpgrade(r,0);if(!ws){printf("upgrade fail %lu\n",GetLastError());return 1;}
 WinHttpSetStatusCallback(ws,cb,WINHTTP_CALLBACK_FLAG_ALL_NOTIFICATIONS,0);
 DWORD t0=GetTickCount(),calls=0,last=0;
 while(GetTickCount()-t0<4000){DWORD e=WinHttpWebSocketReceive(ws,buf,sizeof buf,0,0);calls++;
  if(e!=last){printf("Receive ret=%lu\n",e);last=e;}
  if(e==0)WaitForSingleObject(ev,200);else Sleep(1);}
 printf("calls=%lu reads=%ld errcallbacks=%ld\n",calls,reads,errs);return 0;}
