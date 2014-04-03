# Floats are separately difficult to OSR due to the fact that they use different registers
# and aren't directly bitcast-able to pointers:

def f():
    a = 1.0
    b = 2.0
    c = 3.0
    d = 4.0
    e = 5.0
    f = 6.0

    for i in xrange(20000):
        a = b * c + a
    print a, b, c, d, e, f
f()
