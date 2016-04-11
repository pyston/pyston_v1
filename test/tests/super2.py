# this test is a modfied version of a testcase inside test_descr

class C:
    __metaclass__ = type
    def __init__(self):
	self.__state = 0
    def getstate(self):
	return self.__state
    def setstate(self, state):
	self.__state = state
a = C()
assert a.getstate() == 0
a.setstate(10)
assert a.getstate() == 10
class D:
    class __metaclass__(type):
	def myself(cls): return cls
assert D.myself() == D
d = D()
assert d.__class__ == D
class M1(type):
    def __new__(cls, name, bases, dict):
	dict['__spam__'] = 1
	return type.__new__(cls, name, bases, dict)
class C:
    __metaclass__ = M1
assert C.__spam__ == 1
c = C()
assert c.__spam__ == 1

class _instance(object):
    pass
class M2(object):
    @staticmethod
    def __new__(cls, name, bases, dict):
	self = object.__new__(cls)
	self.name = name
	self.bases = bases
	self.dict = dict
	return self
    def __call__(self):
	it = _instance()
	# Early binding of methods
	for key in self.dict:
	    if key.startswith("__"):
		continue
	    setattr(it, key, self.dict[key].__get__(it, self))
	return it
class C:
    __metaclass__ = M2
    def spam(self):
	return 42
assert C.name == 'C'
assert C.bases == ()
assert 'spam' in C.dict
c = C()
assert c.spam() == 42

# More metaclass examples

class autosuper(type):
    # Automatically add __super to the class
    # This trick only works for dynamic classes
    def __new__(metaclass, name, bases, dict):
	cls = super(autosuper, metaclass).__new__(metaclass,
						  name, bases, dict)
	# Name mangling for __super removes leading underscores
	while name[:1] == "_":
	    name = name[1:]
	if name:
	    name = "_%s__super" % name
	else:
	    name = "__super"
	setattr(cls, name, super(cls))
	return cls
class A:
    __metaclass__ = autosuper
    def meth(self):
	return "A"
class B(A):
    def meth(self):
	return "B" + self.__super.meth()
class C(A):
    def meth(self):
	return "C" + self.__super.meth()
class D(C, B):
    def meth(self):
	return "D" + self.__super.meth()
assert D().meth() == "DCBA"
class E(B, C):
    def meth(self):
	return "E" + self.__super.meth()
assert E().meth() == "EBCA"

class autoproperty(type):
    # Automatically create property attributes when methods
    # named _get_x and/or _set_x are found
    def __new__(metaclass, name, bases, dict):
	hits = {}
	for key, val in dict.iteritems():
	    if key.startswith("_get_"):
		key = key[5:]
		get, set = hits.get(key, (None, None))
		get = val
		hits[key] = get, set
	    elif key.startswith("_set_"):
		key = key[5:]
		get, set = hits.get(key, (None, None))
		set = val
		hits[key] = get, set
	for key, (get, set) in hits.iteritems():
	    dict[key] = property(get, set)
	return super(autoproperty, metaclass).__new__(metaclass,
						    name, bases, dict)
class A:
    __metaclass__ = autoproperty
    def _get_x(self):
	return -self.__x
    def _set_x(self, x):
	self.__x = -x
a = A()
assert not hasattr(a, "x")
a.x = 12
assert a.x == 12
assert a._A__x == -12

class multimetaclass(autoproperty, autosuper):
    # Merge of multiple cooperating metaclasses
    pass
class A:
    __metaclass__ = multimetaclass
    def _get_x(self):
	return "A"
class B(A):
    def _get_x(self):
	return "B" + self.__super._get_x()
class C(A):
    def _get_x(self):
	return "C" + self.__super._get_x()
class D(C, B):
    def _get_x(self):
	return "D" + self.__super._get_x()
assert D().x == "DCBA"

# Make sure type(x) doesn't call x.__class__.__init__
class T(type):
    counter = 0
    def __init__(self, *args):
	T.counter += 1
class C:
    __metaclass__ = T
assert T.counter == 1
a = C()
assert type(a) == C
assert T.counter == 1

class C(object): pass
c = C()
try: c()
except TypeError: pass
else: self.fail("calling object w/o call method should raise "
		"TypeError")

# Testing code to find most derived baseclass
class A(type):
    def __new__(*args, **kwargs):
	return type.__new__(*args, **kwargs)

class B(object):
    pass

class C(object):
    __metaclass__ = A

# The most derived metaclass of D is A rather than type.
class D(B, C):
    pass


print "finished"


