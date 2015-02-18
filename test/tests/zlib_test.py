import zlib

s = "hello world!"
s2 = zlib.compress(s)
print repr(s2)
s3 = zlib.decompress(s2)
print repr(s3)
