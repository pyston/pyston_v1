import ssl
import socket
s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
ssl_sock = ssl.wrap_socket(s)
if 0:
    ssl_sock.connect(("google.com", 443))
    print ssl_sock.getpeercert()
    ssl_sock.write("GET / \n")
    print ssl_sock.read()
    print ssl_sock.getsockname()
ssl_sock.close()
