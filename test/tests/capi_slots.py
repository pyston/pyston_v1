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
print slots_test.SlotsTesterNum(0) == slots_test.SlotsTesterNum(0)
print slots_test.SlotsTesterNum(0) == slots_test.SlotsTesterNum(1)

for i in slots_test.SlotsTesterSeq(6):
    print i

try:
    # seqiter.tp_new is NULL so we should not be allowed to create an instance
    slot_tester_seqiter = type(iter(slots_test.SlotsTesterSeq(6)))
    print slot_tester_seqiter
    slot_tester_seqiter()
except Exception as e:
    print e

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
print c.__dict__.items()

s = slots_test.SlotsTesterMap(6)
s.bar = 2
print s.bar
print hasattr(s, "bar"), hasattr(s, "foo")
try:
    print s.__dict__
except AttributeError, e:
    print e


class C5(C):
    def __getattr__(self, attr):
        print "getattr", attr

c = C5()
slots_test.call_funcs(c)
c.foo
c.bar
c.baz


def _getattr_(self, attr):
    print "_getattr_", attr

class C6(C):
    pass

c = C6()
c.__getattr__ = _getattr_
slots_test.call_funcs(c)

try:
    c.foo
except AttributeError, e:
    print e

c.__getattro__ = _getattr_
slots_test.call_funcs(c)

try:
    c.foo
except AttributeError, e:
    print e

c = slots_test.SlotsTesterNullReturnGetAttr(5)
try:
    print c.foo
except SystemError, e:
    print e

try:
    print c.foo()
except SystemError, e:
    print e


print "**** setattr on class def"
class C7(C):
    def __setattr__(self, attr, val):
        print "setattr", attr, val

c = C7()
slots_test.call_funcs(c)
c.foo = 1
c.bar = 2
c.baz = 3


print "**** setattr set after the fact"
def _setattr_(self, attr, val):
    print "_setattr_", attr, val

c = C6()
c.__setattr__ = _setattr_
slots_test.call_funcs(c)
c.foo = 1
c.bar = 2
c.baz = 3

print "**** delattr on class def"
class C8(C):
    def __delattr__(self, attr):
        print "delattr", attr

c = C8()
slots_test.call_funcs(c)
delattr(c, 'foo')
del c.bar

print "**** delattr set after the fact"
def _delattr_(self, attr):
    print "_delattr_", attr

c = C6()
c.__delattr__ = _delattr_
slots_test.call_funcs(c)
try:
    delattr(c, 'foo')
except Exception as e:
    pass
try:
    del c.bar
except Exception as e:
    pass

slots_test.call_funcs(C())
C.__get__ = lambda *args: None
slots_test.call_funcs(C())
del C.__get__
slots_test.call_funcs(C())

# test if __get__'s called
class C(object):
    val = slots_test.SlotsTesterDescrGet()
print C().val


# Test that extension classes (in this case, weakref.proxy) get our custom class-level flags
# (in this case, has_getattribute)
import weakref
class C(object):
    pass
c = C()
proxy = weakref.proxy(c)
print isinstance(proxy, C)
