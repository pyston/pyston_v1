def gcd(a, b):
    if a > b:
        a, b = b, a

    while a > 0:
        a, b = b % a, a
    return b

for i in xrange(1, 10):
    for j in xrange(1, 10):
        print i, j, gcd(i, j)

def sort(l):
    n = len(l)
    for i in xrange(n):
        for j in xrange(i):
            if l[j] > l[i]:
                l[j], l[i] = l[i], l[j]
    return l
print sort([1, 3, 5, 7, 2, 4, 9, 9, 4])

try:
    a, b, c = 1,
except ValueError, e:
    print e

# Ok this isn't really "tuple" unpacking but it's pretty much the same thing:
[a, [b, c], d] = (1, [3, 4], 5)
print a, b, c, d
