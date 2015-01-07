import struct
s = struct.pack("II", 1, 1234)
print repr(s)
print struct.unpack("II", s)
