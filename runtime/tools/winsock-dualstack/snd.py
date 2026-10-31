import socket,sys
s=socket.socket(socket.AF_INET,socket.SOCK_DGRAM);s.settimeout(4);s.sendto(b"x"*104,("127.0.0.1",22432))
try: print("reply",s.recvfrom(64))
except Exception as e: print("noreply",e)
