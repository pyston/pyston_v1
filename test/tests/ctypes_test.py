from ctypes import *
s = "tmp"
ap = create_string_buffer(s)

print type(ap)
print type(c_void_p.from_param(ap))
print type(cast(ap, c_char_p))
