from collections import namedtuple

NT = namedtuple("NT", "")

def f():
    C = NT
    for i in xrange(1000000):
        C()
        C()
        C()
        C()
        C()
        C()
        C()
        C()
        C()
        C()
f()

