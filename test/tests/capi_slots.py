import slots_test

for i in xrange(3):
    t = slots_test.SlotsTesterSeq(i + 5)
    print t, repr(t), str(t), t(), t[2]
    print hash(t), t < 1, t > 2, t != 3, bool(t)

# print slots_test.SlotsTesterSeq.__doc__
print slots_test.SlotsTesterSeq.set_through_tpdict, slots_test.SlotsTesterSeq(5).set_through_tpdict

for i in xrange(3):
    t = slots_test.SlotsTesterMap(i + 5)
    print len(t), t[2], repr(t), str(t)
    t[1] = 5
    del t[2]

for i in xrange(3):
    t = slots_test.SlotsTesterNum(i)
    print bool(t)
    print t + 5
    print t - 5
    print t * 5
    print t / 5
    print t % 5
    print divmod(t, 5)
    print t ** 5
    print t << 5
    print t >> 5
    print t & 5
    print t ^ 5
    print t | 5
    print +t
    print -t
    print abs(t)
    print ~t
    print int(t)
    print long(t)
    print float(t)
    print hex(t)
    print oct(t)

su = slots_test.SlotsTesterSub(5)
print su

class C(object):
    def __repr__(self):
        print "__repr__()"
        return "<C object>"

    def __getitem__(self, idx):
        print "__getitem__", idx
        return idx - 5

    def __len__(self):
        print "__len__"
        return 3

slots_test.call_funcs(C())

# Test to make sure that updating an existing class also updates the tp_* slots:
def repr2(self):
    return "repr2()"
C.__repr__ = repr2

def nonzero(self):
    print "nonzero"
    return True
C.__nonzero__ = nonzero

def add(self, rhs):
    print "add", self, rhs
C.__add__ = add

slots_test.call_funcs(C())

class C2(C):
    pass

slots_test.call_funcs(C2())

class C3(slots_test.SlotsTesterSeq):
    pass

slots_test.call_funcs(C3(5))

try:
    class C4(slots_test.SlotsTesterNonsubclassable):
        pass
except TypeError, e:
    print e

try:
    slots_test.SlotsTesterSeq(5).foo = 1
except AttributeError, e:
    print e

try:
    print slots_test.SlotsTesterSeq(5).__dict__
except AttributeError, e:
    print e

c = C3(5)
c.foo = 1
print c.foo
print c.__dict__

s = slots_test.SlotsTesterMap(6)
s.bar = 2
print s.bar
print hasattr(s, "bar"), hasattr(s, "foo")
try:
    print s.__dict__
except AttributeError, e:
    print e
