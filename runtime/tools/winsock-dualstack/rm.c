#include <winsock2.h>
#include <ws2tcpip.h>
#include <mswsock.h>
#include <stdio.h>
static LPFN_WSARECVMSG RecvMsg; static LPFN_WSASENDMSG SendMsg;
int main(int argc,char**argv){WSADATA w;WSAStartup(MAKEWORD(2,2),&w);int mode=argc>1?atoi(argv[1]):0;
 SOCKET s=socket(AF_INET6,SOCK_DGRAM,IPPROTO_UDP);DWORD z=0,on=1,n;setsockopt(s,IPPROTO_IPV6,IPV6_V6ONLY,(char*)&z,sizeof z);
 if(mode&1)printf("RECVPKTINFO6=%d\n",setsockopt(s,IPPROTO_IPV6,IPV6_PKTINFO,(char*)&on,sizeof on));
 if(mode&2)printf("RECVPKTINFO4=%d err=%d\n",setsockopt(s,IPPROTO_IP,IP_PKTINFO,(char*)&on,sizeof on),WSAGetLastError());
 struct sockaddr_in6 b={0};b.sin6_family=AF_INET6;b.sin6_port=htons(22432);printf("bind=%d\n",bind(s,(void*)&b,sizeof b));
 GUID g1=WSAID_WSARECVMSG,g2=WSAID_WSASENDMSG;
 WSAIoctl(s,SIO_GET_EXTENSION_FUNCTION_POINTER,&g1,sizeof g1,&RecvMsg,sizeof RecvMsg,&n,0,0);
 WSAIoctl(s,SIO_GET_EXTENSION_FUNCTION_POINTER,&g2,sizeof g2,&SendMsg,sizeof SendMsg,&n,0,0);
 printf("ptrs %p %p\n",RecvMsg,SendMsg);
 char buf[256],ctl[256];struct sockaddr_in6 from;WSABUF wb={sizeof buf,buf};WSAMSG m={0};
 m.name=(void*)&from;m.namelen=sizeof from;m.lpBuffers=&wb;m.dwBufferCount=1;m.Control.buf=ctl;m.Control.len=argc>2?atoi(argv[2]):sizeof ctl;
 DWORD to=6000;setsockopt(s,SOL_SOCKET,SO_RCVTIMEO,(char*)&to,sizeof to);
 int r=RecvMsg(s,&m,&n,0,0);printf("RecvMsg=%d err=%d bytes=%lu namelen=%d ctllen=%lu flags=%lx\n",r,r?WSAGetLastError():0,n,m.namelen,(unsigned long)m.Control.len,(unsigned long)m.dwFlags);
 if(r)return 1;char a[64];InetNtopA(AF_INET6,&from.sin6_addr,a,64);printf("from=%s port=%d fam=%d\n",a,ntohs(from.sin6_port),from.sin6_family);
 for(WSACMSGHDR*c=WSA_CMSG_FIRSTHDR(&m);c;c=WSA_CMSG_NXTHDR(&m,c))printf("cmsg level=%d type=%d len=%lu\n",c->cmsg_level,c->cmsg_type,(unsigned long)c->cmsg_len);
 wb.len=4;memcpy(buf,"pong",4);m.dwBufferCount=1;m.Control.len=0;m.Control.buf=0;
 r=SendMsg(s,&m,0,&n,0,0);printf("SendMsg=%d err=%d bytes=%lu\n",r,r?WSAGetLastError():0,n);return 0;}
