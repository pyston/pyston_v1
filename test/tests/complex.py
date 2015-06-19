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
