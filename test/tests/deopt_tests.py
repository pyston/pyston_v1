# statcheck: 0 <= noninit_count('num_deopt') < 500

def f(o):
    print "starting"

    try:
        print o.a
        if o.b:
            raise Exception()
    except Exception, e:
        print o.c
        print e
    print o.d

    print "Done"

class C(object):
    pass
c = C()
c.a = 1
c.b = 0
c.c = 3
c.d = 4

# These limits are high to try to trigger OSR.
# TODO we should have some way to lower the OSR thresholds
for i in xrange(20000):
    if i == 5000:
        c.a = []

    if i == 6000:
        c.b = 1

    if i == 7000:
        c.c = []

    if i == 8000:
        c.b = 0
        c.d = 1.0

    f(c)
