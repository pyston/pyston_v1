# skip-if: '-O' in EXTRA_JIT_ARGS
# statcheck: 4 <= noninit_count('num_deopt') < 50

def f(o):
    print "starting"

    try:
        print o.a
        if o.b:
            raise Exception('')
    except Exception, e:
        print o.c
        print e
    print o.d
    print sorted(locals().items())

    print "Done"

class C(object):
    def __repr__(self):
        return "<C>"

c = C()
c.a = 1
c.b = 0
c.c = 3
c.d = 4

# These limits are high to try to trigger OSR.
# TODO we should have some way to lower the OSR thresholds
for i in xrange(20000):
    print i

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



# Regression test reduced from subprocess.py:
import types
def f2(self, args):
    if isinstance(args, types.StringTypes):
        pass

    try:
        self.pid
    except:
        pass

c = C()
c.pid = 1
for i in xrange(20000):
    f2(c, None)

    if i == 15000:
        c.pid = 1.0
