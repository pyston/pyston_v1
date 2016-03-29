# TODO repr is wrong, so for now, only printing complex numbers whose real
# and imaginary parts are non-integers

print 0.5j + 1.5
print 0.5j + 1.5
print 0.5j + 1.5

print 0.5j - 1.5
print 0.5j - 1.5
print 0.5j - 1.5

print 0.5j * 1.5
print 0.5j * 1.5
print 0.5j * 1.5

print (0.5j + 1.5).real
print (0.5j + 1.5).imag

print complex(1, 1.0) / 2.0

try:
    complex(1, 1.0).real = 1
except TypeError, e:
    print e

class C(complex):
    pass

print(1j - C(1))
print(1j + C(1))
print(1j * C(1))
print(1j / C(1))

class C1(complex):
    def __complex__(self):
        return 1j

print(C1(1) + 1j)
print(C1(1) - 1j)
print(C1(1) * 1j)
print(C1(1) / 1j)

types = [int, float, long]

for _type in types:
    try:
        _type(1j)
    except TypeError as e:
        print(e.message)

data = ["-1j", "0j", "1j",
        "5+5j", "5-5j",
        "5", "5L",  "5.5",
        "\"5\"", "None",
        ]
operations = ["__mod__", "__rmod__",
              "__divmod__", "__rdivmod__",
              "__truediv__", "__rtruediv__",
              "__floordiv__", "__rfloordiv__",
              ]

for x in data:
    for y in data:
        for operation in operations:
            try:
                print(eval("complex.{op}({arg1}, {arg2})".format(op=operation,
                                                                 arg1=x,
                                                                 arg2=y)))
            except Exception as e:
                print(e.message)
