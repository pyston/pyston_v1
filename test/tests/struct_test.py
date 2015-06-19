import struct
s = struct.pack("II", 1, 1234)
print repr(s)
print struct.unpack("II", s)

nums = [0, 0x98, -0x12, 0x9876, -0x1234, 0x98765432, -0x12345678, 0x9876543212345678, -0x1234567812345678]
for exp in 7, 8, 15, 16, 31, 32, 63, 64:
    nums += [2 ** exp, 2 ** exp - 1, -2 ** exp, -2 ** exp - 1]

for format in "bB?hHiIlLqQP":
    for order in [""] + list("@=<>!"):
        for num in nums:
            try:
                spec = "%s%s" % (order, format)
                print (spec, hex(num)), repr(struct.pack(spec, num))
            except struct.error as e:
                print "struct.error:", e
            except OverflowError as e:
                print "OverflowError:", e
