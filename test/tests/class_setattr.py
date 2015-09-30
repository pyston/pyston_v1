# expected: fail

class C(object):
    pass

# Make sure we can't skirt the tp_slot-updating logic in type.__setattr__
# by trying to use object.__setattr__ which wouldn't do the internal bookkeeping:

def badrepr():
    raise Exception()

c = C()
c.a = 1
try:
    object.__setattr__(C, '__repr__', badrepr)
    assert 0
except TypeError as e:
    print e
c.b = 2

