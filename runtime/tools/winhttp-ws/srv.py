import socket,hashlib,base64,sys,time
mode=sys.argv[1]; s=socket.socket(); s.setsockopt(socket.SOL_SOCKET,socket.SO_REUSEADDR,1)
s.bind(("127.0.0.1",18765)); s.listen(1); c,_=s.accept(); d=b""
while b"\r\n\r\n" not in d: d+=c.recv(4096)
k=[l.split(b": ")[1] for l in d.split(b"\r\n") if l.lower().startswith(b"sec-websocket-key")][0]
a=base64.b64encode(hashlib.sha1(k+b"258EAFA5-E914-47DA-95CA-C5AB0DC85B11").digest())
c.send(b"HTTP/1.1 101 Switching Protocols\r\nConnection: Upgrade\r\nUpgrade: websocket\r\nSec-WebSocket-Accept: "+a+b"\r\n\r\n")
c.send(b"\x81\x02hi"); time.sleep(1)
if mode=="close": c.close()
elif mode=="closeframe": c.send(b"\x88\x02\x03\xe8"); time.sleep(1); c.close()
time.sleep(2)
