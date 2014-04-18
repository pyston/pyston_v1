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

def f2():
    # It is *not* ok to translate "expr += y" into "name = expr; name += y"
    l = range(10)
    l[0] += 5
    print l[0]

    class C(object):
        pass

    c = C()
    c.x = 1
    c.x += 2
    print c.x
f2()

print f(4, 2)
print f(4.1, 2.3)
