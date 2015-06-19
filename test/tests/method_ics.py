# run_args: -n
# statcheck: noninit_count('slowpath_classmethod_get') <= 10
# statcheck: noninit_count('slowpath_staticmethod_get') <= 10
# statcheck: noninit_count('slowpath_instancemethod_get') <= 10

def _f_plain(self, a, b, c, d):
    print 'in f', type(self), a, b, c, d

@staticmethod
def _g(a, b, c, d):
    print 'in g', a, b, c, d

@classmethod
def _h(cls, a, b, c, d):
    print 'in h', cls, a, b, c, d

class C(object):
    f_plain = _f_plain
    g = _g
    h = _h
_f = C.f_plain
C.f = _f

def run():
    c = C()

    c.f(1, 2, 3, 4)
    c.g(1, 2, 3, 4)
    c.h(1, 2, 3, 4)

    f1 = c.f
    f1(1,2,3,4)

    g1 = c.g
    g1(1,2,3,4)

    h1 = c.h
    h1(1,2,3,4)

    _f.__get__(c, C)(1,2,3,4)
    _g.__get__(c, C)(1,2,3,4)
    _h.__get__(c, C)(1,2,3,4)

for i in xrange(1000):
    run()
