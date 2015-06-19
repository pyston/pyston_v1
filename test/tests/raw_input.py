import StringIO
import sys

orig_stdin = sys.stdin
sio = StringIO.StringIO("hello world\nfoo")
sys.stdin = sio
print repr(raw_input())
print repr(raw_input("hi"))
sys.stdin = orig_stdin
