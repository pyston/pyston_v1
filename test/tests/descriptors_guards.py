# All the ways of invalidating getattr

# TODO should test getclsattr as well
# TODO should also test some crazier stuff, like descriptors with inheritance

def get(self, obj, typ):
    print '__get__ called'
    print type(self)
    print type(obj)
    print typ
    return self.elem

def set(self, obj, typ):
    print '__set__ called'
    print type(self)
    print type(obj)
    print typ

class Descriptor(object):
    def __init__(self, elem):
        self.elem = elem
    def __str__(self):
        return 'Descriptor object'

class C(object):
    a = Descriptor(0)
    b = Descriptor(lambda : 0)

c = C()

def f():
    print "c.a:", c.a
    print "C.a:", C.a

def g():
    try:
        print "c.b():", c.b()
    except TypeError:
        print 'got TypeError'

    try:
        print "C.b():", C.b()
    except TypeError:
        print 'got TypeError'

def h():
    c.c = 10

for i in xrange(2000):
    print i

    f()
    g()
    h()

    if i == 50:
        Descriptor.__get__ = get
    if i == 100:
        Descriptor.__set__ = set
    if i == 150:
        del Descriptor.__get__
    if i == 200:
        del Descriptor.__set__

    if i == 250:
        Descriptor.__set__ = set
    if i == 300:
        Descriptor.__get__ = get
    if i == 350:
        del Descriptor.__set__
    if i == 400:
        del Descriptor.__get__

    if i == 450:
        Descriptor.__get__ = get
        Descriptor.__set__ = set
    if i == 500:
        del Descriptor.__get__
        del Descriptor.__set__

    if i == 550:
        Descriptor.__get__ = get
    if i == 600:
        Descriptor.__set__ = set
        del Descriptor.__get__
    if i == 650:
        Descriptor.__get__ = get
        del Descriptor.__set__

    if i == 700:
        c.a = 5
        c.b = lambda : 5
    if i == 750:
        del c.a
        del c.b

    if i == 800:
        Descriptor.__set__ = set
    if i == 850:
        del Descriptor.__set__
        c.a = 5
        c.b = lambda : 5
        Descriptor.__set__ = set
    if i == 900:
        del Descriptor.__set__
        del c.a
        del c.b
        Descriptor.__set__ = set

    if i == 950:
        del Descriptor.__get__
    if i == 1000:
        del Descriptor.__set__
        c.a = 5
        c.b = lambda : 5
        Descriptor.__set__ = set
    if i == 1050:
        del Descriptor.__set__
        del c.a
        del c.b
        Descriptor.__set__ = set

    if i == 1100:
        del Descriptor.__set__
    if i == 1150:
        c.a = 5
        c.b = lambda : 5
    if i == 1200:
        del c.a
        del c.b

    if i == 1250:
        c.a = 5
        c.b = lambda : 5

    if i == 1350:
        Descriptor.__get__ = get
    if i == 1400:
        Descriptor.__set__ = set
    if i == 1450:
        del Descriptor.__get__
    if i == 1500:
        del Descriptor.__set__

    if i == 1550:
        Descriptor.__set__ = set
    if i == 1600:
        Descriptor.__get__ = get
    if i == 1650:
        del Descriptor.__set__
    if i == 1700:
        del Descriptor.__get__

    if i == 1750:
        Descriptor.__get__ = get
        Descriptor.__set__ = set
    if i == 1800:
        del Descriptor.__get__
        del Descriptor.__set__

    if i == 1850:
        Descriptor.__get__ = get
    if i == 1900:
        Descriptor.__set__ = set
        del Descriptor.__get__
    if i == 1950:
        Descriptor.__get__ = get
        del Descriptor.__set__
