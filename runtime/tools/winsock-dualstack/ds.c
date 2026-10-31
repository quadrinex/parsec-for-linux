#include <winsock2.h>
#include <ws2tcpip.h>
#include <stdio.h>
int main(void){WSADATA w;WSAStartup(MAKEWORD(2,2),&w);
 SOCKET s=socket(AF_INET6,SOCK_DGRAM,IPPROTO_UDP); DWORD off=0; int r=setsockopt(s,IPPROTO_IPV6,IPV6_V6ONLY,(char*)&off,sizeof off);printf("v6only=0 -> %d err=%d\n",r,WSAGetLastError());
 struct sockaddr_in6 b={0};b.sin6_family=AF_INET6;b.sin6_port=htons(22431);printf("bind=%d\n",bind(s,(void*)&b,sizeof b));
 struct sockaddr_in6 d={0};d.sin6_family=AF_INET6;d.sin6_port=htons(18766);
 InetPtonA(AF_INET6,"::ffff:127.0.0.1",&d.sin6_addr);
 r=sendto(s,"ping",4,0,(void*)&d,sizeof d);printf("sendto mapped=%d err=%d\n",r,r<0?WSAGetLastError():0);
 DWORD to=2000;setsockopt(s,SOL_SOCKET,SO_RCVTIMEO,(char*)&to,sizeof to);
 char buf[64];struct sockaddr_in6 f;int fl=sizeof f;r=recvfrom(s,buf,64,0,(void*)&f,&fl);
 char a[64]="";InetNtopA(AF_INET6,&f.sin6_addr,a,64);printf("recv=%d from=%s fl=%d err=%d\n",r,a,fl,r<0?WSAGetLastError():0);
 /* getaddrinfo order */
 struct addrinfo h={0},*res,*p;h.ai_socktype=SOCK_DGRAM;h.ai_family=AF_UNSPEC;
 r=getaddrinfo("stun.parsec.app",0,&h,&res);printf("gai=%d\n",r);
 for(p=res;p;p=p->ai_next){char x[64];DWORD l=64;WSAAddressToStringA(p->ai_addr,p->ai_addrlen,0,x,&l);printf(" fam=%d %s\n",p->ai_family,x);}
 return 0;}
