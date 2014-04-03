# Augassign

def f(a, b):
    a += b
    a -= b
    a *= b
    a /= b
    a //= b
    a **= b
    a %= b

    a |= b
    a &= b
    a ^= b
    a <<= b
    a >>= b
    return a

l = range(10)
l2 = l
l += l
print l
print l2

l = range(10)
l2 = l
l = l + l
print l
print l2

print f(4, 2)
print f(4.1, 2.3)
