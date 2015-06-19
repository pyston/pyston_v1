# expected: fail
import __builtin__
import gc
import sys
import types
import unittest
import weakref

from copy import deepcopy
from test import test_support


class OperatorsTest(unittest.TestCase):

    def __init__(self, *args, **kwargs):
        unittest.TestCase.__init__(self, *args, **kwargs)
        self.binops = {
            'add': '+',
            'sub': '-',
            'mul': '*',
            'div': '/',
            'divmod': 'divmod',
            'pow': '**',
            'lshift': '<<',
            'rshift': '>>',
            'and': '&',
            'xor': '^',
            'or': '|',
            'cmp': 'cmp',
            'lt': '<',
            'le': '<=',
            'eq': '==',
            'ne': '!=',
            'gt': '>',
            'ge': '>=',
        }

        for name, expr in self.binops.items():
            if expr.islower():
                expr = expr + "(a, b)"
            else:
                expr = 'a %s b' % expr
            self.binops[name] = expr

        self.unops = {
            'pos': '+',
            'neg': '-',
            'abs': 'abs',
            'invert': '~',
            'int': 'int',
            'long': 'long',
            'float': 'float',
            'oct': 'oct',
            'hex': 'hex',
        }

        for name, expr in self.unops.items():
            if expr.islower():
                expr = expr + "(a)"
            else:
                expr = '%s a' % expr
            self.unops[name] = expr

    def unop_test(self, a, res, expr="len(a)", meth="__len__"):
        d = {'a': a}
        self.assertEqual(eval(expr, d), res)
        t = type(a)
        m = getattr(t, meth)

        # Find method in parent class
        while meth not in t.__dict__:
            t = t.__bases__[0]
        # in some implementations (e.g. PyPy), 'm' can be a regular unbound
        # method object; the getattr() below obtains its underlying function.
        self.assertEqual(getattr(m, 'im_func', m), t.__dict__[meth])
        self.assertEqual(m(a), res)
        bm = getattr(a, meth)
        self.assertEqual(bm(), res)

    def binop_test(self, a, b, res, expr="a+b", meth="__add__"):
        d = {'a': a, 'b': b}

        # XXX Hack so this passes before 2.3 when -Qnew is specified.
        if meth == "__div__" and 1/2 == 0.5:
            meth = "__truediv__"

        if meth == '__divmod__': pass

        self.assertEqual(eval(expr, d), res)
        t = type(a)
        m = getattr(t, meth)
        while meth not in t.__dict__:
            t = t.__bases__[0]
        # in some implementations (e.g. PyPy), 'm' can be a regular unbound
        # method object; the getattr() below obtains its underlying function.
        self.assertEqual(getattr(m, 'im_func', m), t.__dict__[meth])
        self.assertEqual(m(a, b), res)
        bm = getattr(a, meth)
        self.assertEqual(bm(b), res)

    def ternop_test(self, a, b, c, res, expr="a[b:c]", meth="__getslice__"):
        d = {'a': a, 'b': b, 'c': c}
        self.assertEqual(eval(expr, d), res)
        t = type(a)
        m = getattr(t, meth)
        while meth not in t.__dict__:
            t = t.__bases__[0]
        # in some implementations (e.g. PyPy), 'm' can be a regular unbound
        # method object; the getattr() below obtains its underlying function.
        self.assertEqual(getattr(m, 'im_func', m), t.__dict__[meth])
        self.assertEqual(m(a, b, c), res)
        bm = getattr(a, meth)
        self.assertEqual(bm(b, c), res)

    def setop_test(self, a, b, res, stmt="a+=b", meth="__iadd__"):
        d = {'a': deepcopy(a), 'b': b}
        exec stmt in d
        self.assertEqual(d['a'], res)
        t = type(a)
        m = getattr(t, meth)
        while meth not in t.__dict__:
            t = t.__bases__[0]
        # in some implementations (e.g. PyPy), 'm' can be a regular unbound
        # method object; the getattr() below obtains its underlying function.
        self.assertEqual(getattr(m, 'im_func', m), t.__dict__[meth])
        d['a'] = deepcopy(a)
        m(d['a'], b)
        self.assertEqual(d['a'], res)
        d['a'] = deepcopy(a)
        bm = getattr(d['a'], meth)
        bm(b)
        self.assertEqual(d['a'], res)

    def set2op_test(self, a, b, c, res, stmt="a[b]=c", meth="__setitem__"):
        d = {'a': deepcopy(a), 'b': b, 'c': c}
        exec stmt in d
        self.assertEqual(d['a'], res)
        t = type(a)
        m = getattr(t, meth)
        while meth not in t.__dict__:
            t = t.__bases__[0]
        # in some implementations (e.g. PyPy), 'm' can be a regular unbound
        # method object; the getattr() below obtains its underlying function.
        self.assertEqual(getattr(m, 'im_func', m), t.__dict__[meth])
        d['a'] = deepcopy(a)
        m(d['a'], b, c)
        self.assertEqual(d['a'], res)
        d['a'] = deepcopy(a)
        bm = getattr(d['a'], meth)
        bm(b, c)
        self.assertEqual(d['a'], res)

    def set3op_test(self, a, b, c, d, res, stmt="a[b:c]=d", meth="__setslice__"):
        dictionary = {'a': deepcopy(a), 'b': b, 'c': c, 'd': d}
        exec stmt in dictionary
        self.assertEqual(dictionary['a'], res)
        t = type(a)
        while meth not in t.__dict__:
            t = t.__bases__[0]
        m = getattr(t, meth)
        # in some implementations (e.g. PyPy), 'm' can be a regular unbound
        # method object; the getattr() below obtains its underlying function.
        self.assertEqual(getattr(m, 'im_func', m), t.__dict__[meth])
        dictionary['a'] = deepcopy(a)
        m(dictionary['a'], b, c, d)
        self.assertEqual(dictionary['a'], res)
        dictionary['a'] = deepcopy(a)
        bm = getattr(dictionary['a'], meth)
        bm(b, c, d)
        self.assertEqual(dictionary['a'], res)

    def test_lists(self):
        # Testing list operations...
        # Asserts are within individual test methods
        self.binop_test([1], [2], [1,2], "a+b", "__add__")
        self.binop_test([1,2,3], 2, 1, "b in a", "__contains__")
        self.binop_test([1,2,3], 4, 0, "b in a", "__contains__")
        self.binop_test([1,2,3], 1, 2, "a[b]", "__getitem__")
        self.ternop_test([1,2,3], 0, 2, [1,2], "a[b:c]", "__getslice__")
        self.setop_test([1], [2], [1,2], "a+=b", "__iadd__")
        self.setop_test([1,2], 3, [1,2,1,2,1,2], "a*=b", "__imul__")
        self.unop_test([1,2,3], 3, "len(a)", "__len__")
        self.binop_test([1,2], 3, [1,2,1,2,1,2], "a*b", "__mul__")
        self.binop_test([1,2], 3, [1,2,1,2,1,2], "b*a", "__rmul__")
        self.set2op_test([1,2], 1, 3, [1,3], "a[b]=c", "__setitem__")
        self.set3op_test([1,2,3,4], 1, 3, [5,6], [1,5,6,4], "a[b:c]=d",
                        "__setslice__")

    def test_dicts(self):
        # Testing dict operations...
        if hasattr(dict, '__cmp__'):   # PyPy has only rich comparison on dicts
            self.binop_test({1:2}, {2:1}, -1, "cmp(a,b)", "__cmp__")
        else:
            self.binop_test({1:2}, {2:1}, True, "a < b", "__lt__")
        self.binop_test({1:2,3:4}, 1, 1, "b in a", "__contains__")
        self.binop_test({1:2,3:4}, 2, 0, "b in a", "__contains__")
        self.binop_test({1:2,3:4}, 1, 2, "a[b]", "__getitem__")

        d = {1:2, 3:4}
        l1 = []
        for i in d.keys():
            l1.append(i)
        l = []
        for i in iter(d):
            l.append(i)
        self.assertEqual(l, l1)
        l = []
        for i in d.__iter__():
            l.append(i)
        self.assertEqual(l, l1)
        l = []
        for i in dict.__iter__(d):
            l.append(i)
        self.assertEqual(l, l1)
        d = {1:2, 3:4}
        self.unop_test(d, 2, "len(a)", "__len__")
        self.assertEqual(eval(repr(d), {}), d)
        self.assertEqual(eval(d.__repr__(), {}), d)
        self.set2op_test({1:2,3:4}, 2, 3, {1:2,2:3,3:4}, "a[b]=c",
                        "__setitem__")

    # Tests for unary and binary operators
    def number_operators(self, a, b, skip=[]):
        dict = {'a': a, 'b': b}

        for name, expr in self.binops.items():
            if name not in skip:
                name = "__%s__" % name
                if hasattr(a, name):
                    res = eval(expr, dict)
                    self.binop_test(a, b, res, expr, name)

        for name, expr in self.unops.items():
            if name not in skip:
                name = "__%s__" % name
                if hasattr(a, name):
                    res = eval(expr, dict)
                    self.unop_test(a, res, expr, name)

    def test_ints(self):
        # Testing int operations...
        self.number_operators(100, 3)
        # The following crashes in Python 2.2
        self.assertEqual((1).__nonzero__(), 1)
        self.assertEqual((0).__nonzero__(), 0)
        # This returns 'NotImplemented' in Python 2.2
        class C(int):
            def __add__(self, other):
                return NotImplemented
        self.assertEqual(C(5L), 5)
        try:
            C() + ""
        except TypeError:
            pass
        else:
            self.fail("NotImplemented should have caused TypeError")
        try:
            C(sys.maxint+1)
        except OverflowError:
            pass
        else:
            self.fail("should have raised OverflowError")

    def test_longs(self):
        # Testing long operations...
        self.number_operators(100L, 3L)

    def test_floats(self):
        # Testing float operations...
        self.number_operators(100.0, 3.0)

    def test_complexes(self):
        # Testing complex operations...
        self.number_operators(100.0j, 3.0j, skip=['lt', 'le', 'gt', 'ge',
                                                  'int', 'long', 'float'])

        class Number(complex):
            __slots__ = ['prec']
            def __new__(cls, *args, **kwds):
                result = complex.__new__(cls, *args)
                result.prec = kwds.get('prec', 12)
                return result
            def __repr__(self):
                prec = self.prec
                if self.imag == 0.0:
                    return "%.*g" % (prec, self.real)
                if self.real == 0.0:
                    return "%.*gj" % (prec, self.imag)
                return "(%.*g+%.*gj)" % (prec, self.real, prec, self.imag)
            __str__ = __repr__

        a = Number(3.14, prec=6)
        self.assertEqual(repr(a), "3.14")
        self.assertEqual(a.prec, 6)

        a = Number(a, prec=2)
        self.assertEqual(repr(a), "3.1")
        self.assertEqual(a.prec, 2)

        a = Number(234.5)
        self.assertEqual(repr(a), "234.5")
        self.assertEqual(a.prec, 12)

    @test_support.impl_detail("the module 'xxsubtype' is internal")
    def test_spam_lists(self):
        # Testing spamlist operations...
        import copy, xxsubtype as spam

        def spamlist(l, memo=None):
            import xxsubtype as spam
            return spam.spamlist(l)

        # This is an ugly hack:
        copy._deepcopy_dispatch[spam.spamlist] = spamlist

        self.binop_test(spamlist([1]), spamlist([2]), spamlist([1,2]), "a+b",
                       "__add__")
        self.binop_test(spamlist([1,2,3]), 2, 1, "b in a", "__contains__")
        self.binop_test(spamlist([1,2,3]), 4, 0, "b in a", "__contains__")
        self.binop_test(spamlist([1,2,3]), 1, 2, "a[b]", "__getitem__")
        self.ternop_test(spamlist([1,2,3]), 0, 2, spamlist([1,2]), "a[b:c]",
                        "__getslice__")
        self.setop_test(spamlist([1]), spamlist([2]), spamlist([1,2]), "a+=b",
                       "__iadd__")
        self.setop_test(spamlist([1,2]), 3, spamlist([1,2,1,2,1,2]), "a*=b",
                       "__imul__")
        self.unop_test(spamlist([1,2,3]), 3, "len(a)", "__len__")
        self.binop_test(spamlist([1,2]), 3, spamlist([1,2,1,2,1,2]), "a*b",
                       "__mul__")
        self.binop_test(spamlist([1,2]), 3, spamlist([1,2,1,2,1,2]), "b*a",
                       "__rmul__")
        self.set2op_test(spamlist([1,2]), 1, 3, spamlist([1,3]), "a[b]=c",
                        "__setitem__")
        self.set3op_test(spamlist([1,2,3,4]), 1, 3, spamlist([5,6]),
                   spamlist([1,5,6,4]), "a[b:c]=d", "__setslice__")
        # Test subclassing
        class C(spam.spamlist):
            def foo(self): return 1
        a = C()
        self.assertEqual(a, [])
        self.assertEqual(a.foo(), 1)
        a.append(100)
        self.assertEqual(a, [100])
        self.assertEqual(a.getstate(), 0)
        a.setstate(42)
        self.assertEqual(a.getstate(), 42)

    @test_support.impl_detail("the module 'xxsubtype' is internal")
    def test_spam_dicts(self):
        # Testing spamdict operations...
        import copy, xxsubtype as spam
        def spamdict(d, memo=None):
            import xxsubtype as spam
            sd = spam.spamdict()
            for k, v in d.items():
                sd[k] = v
            return sd
        # This is an ugly hack:
        copy._deepcopy_dispatch[spam.spamdict] = spamdict

        self.binop_test(spamdict({1:2}), spamdict({2:1}), -1, "cmp(a,b)",
                       "__cmp__")
        self.binop_test(spamdict({1:2,3:4}), 1, 1, "b in a", "__contains__")
        self.binop_test(spamdict({1:2,3:4}), 2, 0, "b in a", "__contains__")
        self.binop_test(spamdict({1:2,3:4}), 1, 2, "a[b]", "__getitem__")
        d = spamdict({1:2,3:4})
        l1 = []
        for i in d.keys():
            l1.append(i)
        l = []
        for i in iter(d):
            l.append(i)
        self.assertEqual(l, l1)
        l = []
        for i in d.__iter__():
            l.append(i)
        self.assertEqual(l, l1)
        l = []
        for i in type(spamdict({})).__iter__(d):
            l.append(i)
        self.assertEqual(l, l1)
        straightd = {1:2, 3:4}
        spamd = spamdict(straightd)
        self.unop_test(spamd, 2, "len(a)", "__len__")
        self.unop_test(spamd, repr(straightd), "repr(a)", "__repr__")
        self.set2op_test(spamdict({1:2,3:4}), 2, 3, spamdict({1:2,2:3,3:4}),
                   "a[b]=c", "__setitem__")
        # Test subclassing
        class C(spam.spamdict):
            def foo(self): return 1
        a = C()
        self.assertEqual(a.items(), [])
        self.assertEqual(a.foo(), 1)
        a['foo'] = 'bar'
        self.assertEqual(a.items(), [('foo', 'bar')])
        self.assertEqual(a.getstate(), 0)
        a.setstate(100)
        self.assertEqual(a.getstate(), 100)

class ClassPropertiesAndMethods(unittest.TestCase):

    def assertHasAttr(self, obj, name):
        self.assertTrue(hasattr(obj, name),
                        '%r has no attribute %r' % (obj, name))

    def assertNotHasAttr(self, obj, name):
        self.assertFalse(hasattr(obj, name),
                         '%r has unexpected attribute %r' % (obj, name))

    def test_python_dicts(self):
        # Testing Python subclass of dict...
        self.assertTrue(issubclass(dict, dict))
        self.assertIsInstance({}, dict)
        d = dict()
        self.assertEqual(d, {})
        self.assertIs(d.__class__, dict)
        self.assertIsInstance(d, dict)
        class C(dict):
            state = -1
            def __init__(self_local, *a, **kw):
                if a:
                    self.assertEqual(len(a), 1)
                    self_local.state = a[0]
                if kw:
                    for k, v in kw.items():
                        self_local[v] = k
            def __getitem__(self, key):
                return self.get(key, 0)
            def __setitem__(self_local, key, value):
                self.assertIsInstance(key, type(0))
                dict.__setitem__(self_local, key, value)
            def setstate(self, state):
                self.state = state
            def getstate(self):
                return self.state
        self.assertTrue(issubclass(C, dict))
        a1 = C(12)
        self.assertEqual(a1.state, 12)
        a2 = C(foo=1, bar=2)
        self.assertEqual(a2[1] == 'foo' and a2[2], 'bar')
        a = C()
        self.assertEqual(a.state, -1)
        self.assertEqual(a.getstate(), -1)
        a.setstate(0)
        self.assertEqual(a.state, 0)
        self.assertEqual(a.getstate(), 0)
        a.setstate(10)
        self.assertEqual(a.state, 10)
        self.assertEqual(a.getstate(), 10)
        self.assertEqual(a[42], 0)
        a[42] = 24
        self.assertEqual(a[42], 24)
        N = 50
        for i in range(N):
            a[i] = C()
            for j in range(N):
                a[i][j] = i*j
        for i in range(N):
            for j in range(N):
                self.assertEqual(a[i][j], i*j)

    def test_python_lists(self):
        # Testing Python subclass of list...
        class C(list):
            def __getitem__(self, i):
                return list.__getitem__(self, i) + 100
            def __getslice__(self, i, j):
                return (i, j)
        a = C()
        a.extend([0,1,2])
        self.assertEqual(a[0], 100)
        self.assertEqual(a[1], 101)
        self.assertEqual(a[2], 102)
        self.assertEqual(a[100:200], (100,200))

    def test_metaclass(self):
        # Testing __metaclass__...
        class C:
            __metaclass__ = type
            def __init__(self):
                self.__state = 0
            def getstate(self):
                return self.__state
            def setstate(self, state):
                self.__state = state
        a = C()
        self.assertEqual(a.getstate(), 0)
        a.setstate(10)
        self.assertEqual(a.getstate(), 10)
        class D:
            class __metaclass__(type):
                def myself(cls): return cls
        self.assertEqual(D.myself(), D)
        d = D()
        self.assertEqual(d.__class__, D)
        class M1(type):
            def __new__(cls, name, bases, dict):
                dict['__spam__'] = 1
                return type.__new__(cls, name, bases, dict)
        class C:
            __metaclass__ = M1
        self.assertEqual(C.__spam__, 1)
        c = C()
        self.assertEqual(c.__spam__, 1)

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
        self.assertEqual(C.name, 'C')
        self.assertEqual(C.bases, ())
        self.assertIn('spam', C.dict)
        c = C()
        self.assertEqual(c.spam(), 42)

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
        self.assertEqual(D().meth(), "DCBA")
        class E(B, C):
            def meth(self):
                return "E" + self.__super.meth()
        self.assertEqual(E().meth(), "EBCA")

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
        self.assertNotHasAttr(a, "x")
        a.x = 12
        self.assertEqual(a.x, 12)
        self.assertEqual(a._A__x, -12)

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
        self.assertEqual(D().x, "DCBA")

        # Make sure type(x) doesn't call x.__class__.__init__
        class T(type):
            counter = 0
            def __init__(self, *args):
                T.counter += 1
        class C:
            __metaclass__ = T
        self.assertEqual(T.counter, 1)
        a = C()
        self.assertEqual(type(a), C)
        self.assertEqual(T.counter, 1)

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

    def test_module_subclasses(self):
        # Testing Python subclass of module...
        log = []
        MT = type(sys)
        class MM(MT):
            def __init__(self, name):
                MT.__init__(self, name)
            def __getattribute__(self, name):
                log.append(("getattr", name))
                return MT.__getattribute__(self, name)
            def __setattr__(self, name, value):
                log.append(("setattr", name, value))
                MT.__setattr__(self, name, value)
            def __delattr__(self, name):
                log.append(("delattr", name))
                MT.__delattr__(self, name)
        a = MM("a")
        a.foo = 12
        x = a.foo
        del a.foo
        self.assertEqual(log, [("setattr", "foo", 12),
                               ("getattr", "foo"),
                               ("delattr", "foo")])

        # http://python.org/sf/1174712
        try:
            class Module(types.ModuleType, str):
                pass
        except TypeError:
            pass
        else:
            self.fail("inheriting from ModuleType and str at the same time "
                      "should fail")

    def test_multiple_inheritence(self):
        # Testing multiple inheritance...
        class C(object):
            def __init__(self):
                self.__state = 0
            def getstate(self):
                return self.__state
            def setstate(self, state):
                self.__state = state
        a = C()
        self.assertEqual(a.getstate(), 0)
        a.setstate(10)
        self.assertEqual(a.getstate(), 10)
        class D(dict, C):
            def __init__(self):
                type({}).__init__(self)
                C.__init__(self)
        d = D()
        self.assertEqual(d.keys(), [])
        d["hello"] = "world"
        self.assertEqual(d.items(), [("hello", "world")])
        self.assertEqual(d["hello"], "world")
        self.assertEqual(d.getstate(), 0)
        d.setstate(10)
        self.assertEqual(d.getstate(), 10)
        self.assertEqual(D.__mro__, (D, dict, C, object))

        # SF bug #442833
        class Node(object):
            def __int__(self):
                return int(self.foo())
            def foo(self):
                return "23"
        class Frag(Node, list):
            def foo(self):
                return "42"
        self.assertEqual(Node().__int__(), 23)
        self.assertEqual(int(Node()), 23)
        self.assertEqual(Frag().__int__(), 42)
        self.assertEqual(int(Frag()), 42)

        # MI mixing classic and new-style classes.

        class A:
            x = 1

        class B(A):
            pass

        class C(A):
            x = 2

        class D(B, C):
            pass
        self.assertEqual(D.x, 1)

        # Classic MRO is preserved for a classic base class.
        class E(D, object):
            pass
        self.assertEqual(E.__mro__, (E, D, B, A, C, object))
        self.assertEqual(E.x, 1)

        # But with a mix of classic bases, their MROs are combined using
        # new-style MRO.
        class F(B, C, object):
            pass
        self.assertEqual(F.__mro__, (F, B, C, A, object))
        self.assertEqual(F.x, 2)

        # Try something else.
        class C:
            def cmethod(self):
                return "C a"
            def all_method(self):
                return "C b"

        class M1(C, object):
            def m1method(self):
                return "M1 a"
            def all_method(self):
                return "M1 b"

        self.assertEqual(M1.__mro__, (M1, C, object))
        m = M1()
        self.assertEqual(m.cmethod(), "C a")
        self.assertEqual(m.m1method(), "M1 a")
        self.assertEqual(m.all_method(), "M1 b")

        class D(C):
            def dmethod(self):
                return "D a"
            def all_method(self):
                return "D b"

        class M2(D, object):
            def m2method(self):
                return "M2 a"
            def all_method(self):
                return "M2 b"

        self.assertEqual(M2.__mro__, (M2, D, C, object))
        m = M2()
        self.assertEqual(m.cmethod(), "C a")
        self.assertEqual(m.dmethod(), "D a")
        self.assertEqual(m.m2method(), "M2 a")
        self.assertEqual(m.all_method(), "M2 b")

        class M3(M1, M2, object):
            def m3method(self):
                return "M3 a"
            def all_method(self):
                return "M3 b"
        self.assertEqual(M3.__mro__, (M3, M1, M2, D, C, object))
        m = M3()
        self.assertEqual(m.cmethod(), "C a")
        self.assertEqual(m.dmethod(), "D a")
        self.assertEqual(m.m1method(), "M1 a")
        self.assertEqual(m.m2method(), "M2 a")
        self.assertEqual(m.m3method(), "M3 a")
        self.assertEqual(m.all_method(), "M3 b")

        class Classic:
            pass
        try:
            class New(Classic):
                __metaclass__ = type
        except TypeError:
            pass
        else:
            self.fail("new class with only classic bases - shouldn't be")

    def test_diamond_inheritence(self):
        # Testing multiple inheritance special cases...
        class A(object):
            def spam(self): return "A"
        self.assertEqual(A().spam(), "A")
        class B(A):
            def boo(self): return "B"
            def spam(self): return "B"
        self.assertEqual(B().spam(), "B")
        self.assertEqual(B().boo(), "B")
        class C(A):
            def boo(self): return "C"
        self.assertEqual(C().spam(), "A")
        self.assertEqual(C().boo(), "C")
        class D(B, C): pass
        self.assertEqual(D().spam(), "B")
        self.assertEqual(D().boo(), "B")
        self.assertEqual(D.__mro__, (D, B, C, A, object))
        class E(C, B): pass
        self.assertEqual(E().spam(), "B")
        self.assertEqual(E().boo(), "C")
        self.assertEqual(E.__mro__, (E, C, B, A, object))
        # MRO order disagreement
        try:
            class F(D, E): pass
        except TypeError:
            pass
        else:
            self.fail("expected MRO order disagreement (F)")
        try:
            class G(E, D): pass
        except TypeError:
            pass
        else:
            self.fail("expected MRO order disagreement (G)")

    # see thread python-dev/2002-October/029035.html
    def test_ex5_from_c3_switch(self):
        # Testing ex5 from C3 switch discussion...
        class A(object): pass
        class B(object): pass
        class C(object): pass
        class X(A): pass
        class Y(A): pass
        class Z(X,B,Y,C): pass
        self.assertEqual(Z.__mro__, (Z, X, B, Y, A, C, object))

    # see "A Monotonic Superclass Linearization for Dylan",
    # by Kim Barrett et al. (OOPSLA 1996)
    def test_monotonicity(self):
        # Testing MRO monotonicity...
        class Boat(object): pass
        class DayBoat(Boat): pass
        class WheelBoat(Boat): pass
        class EngineLess(DayBoat): pass
        class SmallMultihull(DayBoat): pass
        class PedalWheelBoat(EngineLess,WheelBoat): pass
        class SmallCatamaran(SmallMultihull): pass
        class Pedalo(PedalWheelBoat,SmallCatamaran): pass

        self.assertEqual(PedalWheelBoat.__mro__,
              (PedalWheelBoat, EngineLess, DayBoat, WheelBoat, Boat, object))
        self.assertEqual(SmallCatamaran.__mro__,
              (SmallCatamaran, SmallMultihull, DayBoat, Boat, object))
        self.assertEqual(Pedalo.__mro__,
              (Pedalo, PedalWheelBoat, EngineLess, SmallCatamaran,
               SmallMultihull, DayBoat, WheelBoat, Boat, object))

    # see "A Monotonic Superclass Linearization for Dylan",
    # by Kim Barrett et al. (OOPSLA 1996)
    def test_consistency_with_epg(self):
        # Testing consistency with EPG...
        class Pane(object): pass
        class ScrollingMixin(object): pass
        class EditingMixin(object): pass
        class ScrollablePane(Pane,ScrollingMixin): pass
        class EditablePane(Pane,EditingMixin): pass
        class EditableScrollablePane(ScrollablePane,EditablePane): pass

        self.assertEqual(EditableScrollablePane.__mro__,
              (EditableScrollablePane, ScrollablePane, EditablePane, Pane,
                ScrollingMixin, EditingMixin, object))

    def test_mro_disagreement(self):
        # Testing error messages for MRO disagreement...
        mro_err_msg = """Cannot create a consistent method resolution
order (MRO) for bases """

        def raises(exc, expected, callable, *args):
            try:
                callable(*args)
            except exc, msg:
                # the exact msg is generally considered an impl detail
                if test_support.check_impl_detail():
                    if not str(msg).startswith(expected):
                        self.fail("Message %r, expected %r" %
                                  (str(msg), expected))
            else:
                self.fail("Expected %s" % exc)

        class A(object): pass
        class B(A): pass
        class C(object): pass

        # Test some very simple errors
        raises(TypeError, "duplicate base class A",
               type, "X", (A, A), {})
        raises(TypeError, mro_err_msg,
               type, "X", (A, B), {})
        raises(TypeError, mro_err_msg,
               type, "X", (A, C, B), {})
        # Test a slightly more complex error
        class GridLayout(object): pass
        class HorizontalGrid(GridLayout): pass
        class VerticalGrid(GridLayout): pass
        class HVGrid(HorizontalGrid, VerticalGrid): pass
        class VHGrid(VerticalGrid, HorizontalGrid): pass
        raises(TypeError, mro_err_msg,
               type, "ConfusedGrid", (HVGrid, VHGrid), {})

    def test_object_class(self):
        # Testing object class...
        a = object()
        self.assertEqual(a.__class__, object)
        self.assertEqual(type(a), object)
        b = object()
        self.assertNotEqual(a, b)
        self.assertNotHasAttr(a, "foo")
        try:
            a.foo = 12
        except (AttributeError, TypeError):
            pass
        else:
            self.fail("object() should not allow setting a foo attribute")
        self.assertNotHasAttr(object(), "__dict__")

        class Cdict(object):
            pass
        x = Cdict()
        self.assertEqual(x.__dict__, {})
        x.foo = 1
        self.assertEqual(x.foo, 1)
        self.assertEqual(x.__dict__, {'foo': 1})

    def test_slots(self):
        # Testing __slots__...
        class C0(object):
            __slots__ = []
        x = C0()
        self.assertNotHasAttr(x, "__dict__")
        self.assertNotHasAttr(x, "foo")

        class C1(object):
            __slots__ = ['a']
        x = C1()
        self.assertNotHasAttr(x, "__dict__")
        self.assertNotHasAttr(x, "a")
        x.a = 1
        self.assertEqual(x.a, 1)
        x.a = None
        self.assertEqual(x.a, None)
        del x.a
        self.assertNotHasAttr(x, "a")

        class C3(object):
            __slots__ = ['a', 'b', 'c']
        x = C3()
        self.assertNotHasAttr(x, "__dict__")
        self.assertNotHasAttr(x, 'a')
        self.assertNotHasAttr(x, 'b')
        self.assertNotHasAttr(x, 'c')
        x.a = 1
        x.b = 2
        x.c = 3
        self.assertEqual(x.a, 1)
        self.assertEqual(x.b, 2)
        self.assertEqual(x.c, 3)

        class C4(object):
            """Validate name mangling"""
            __slots__ = ['__a']
            def __init__(self, value):
                self.__a = value
            def get(self):
                return self.__a
        x = C4(5)
        self.assertNotHasAttr(x, '__dict__')
        self.assertNotHasAttr(x, '__a')
        self.assertEqual(x.get(), 5)
        try:
            x.__a = 6
        except AttributeError:
            pass
        else:
            self.fail("Double underscored names not mangled")

        # Make sure slot names are proper identifiers
        try:
            class C(object):
                __slots__ = [None]
        except TypeError:
            pass
        else:
            self.fail("[None] slots not caught")
        try:
            class C(object):
                __slots__ = ["foo bar"]
        except TypeError:
            pass
        else:
            self.fail("['foo bar'] slots not caught")
        try:
            class C(object):
                __slots__ = ["foo\0bar"]
        except TypeError:
            pass
        else:
            self.fail("['foo\\0bar'] slots not caught")
        try:
            class C(object):
                __slots__ = ["1"]
        except TypeError:
            pass
        else:
            self.fail("['1'] slots not caught")
        try:
            class C(object):
                __slots__ = [""]
        except TypeError:
            pass
        else:
            self.fail("[''] slots not caught")
        class C(object):
            __slots__ = ["a", "a_b", "_a", "A0123456789Z"]
        # XXX(nnorwitz): was there supposed to be something tested
        # from the class above?

        # Test a single string is not expanded as a sequence.
        class C(object):
            __slots__ = "abc"
        c = C()
        c.abc = 5
        self.assertEqual(c.abc, 5)

    def test_unicode_slots(self):
        # Test unicode slot names
        try:
            unicode
        except NameError:
            self.skipTest('no unicode support')
        else:
            # Test a single unicode string is not expanded as a sequence.
            class C(object):
                __slots__ = unicode("abc")
            c = C()
            c.abc = 5
            self.assertEqual(c.abc, 5)

            # _unicode_to_string used to modify slots in certain circumstances
            slots = (unicode("foo"), unicode("bar"))
            class C(object):
                __slots__ = slots
            x = C()
            x.foo = 5
            self.assertEqual(x.foo, 5)
            self.assertEqual(type(slots[0]), unicode)
            # this used to leak references
            try:
                class C(object):
                    __slots__ = [unichr(128)]
            except (TypeError, UnicodeEncodeError):
                pass
            else:
                self.fail("[unichr(128)] slots not caught")

        # Test leaks
        class Counted(object):
            counter = 0    # counts the number of instances alive
            def __init__(self):
                Counted.counter += 1
            def __del__(self):
                Counted.counter -= 1
        class C(object):
            __slots__ = ['a', 'b', 'c']
        x = C()
        x.a = Counted()
        x.b = Counted()
        x.c = Counted()
        self.assertEqual(Counted.counter, 3)
        del x
        test_support.gc_collect()
        self.assertEqual(Counted.counter, 0)
        class D(C):
            pass
        x = D()
        x.a = Counted()
        x.z = Counted()
        self.assertEqual(Counted.counter, 2)
        del x
        test_support.gc_collect()
        self.assertEqual(Counted.counter, 0)
        class E(D):
            __slots__ = ['e']
        x = E()
        x.a = Counted()
        x.z = Counted()
        x.e = Counted()
        self.assertEqual(Counted.counter, 3)
        del x
        test_support.gc_collect()
        self.assertEqual(Counted.counter, 0)

        # Test cyclical leaks [SF bug 519621]
        class F(object):
            __slots__ = ['a', 'b']
        s = F()
        s.a = [Counted(), s]
        self.assertEqual(Counted.counter, 1)
        s = None
        test_support.gc_collect()
        self.assertEqual(Counted.counter, 0)

        # Test lookup leaks [SF bug 572567]
        if hasattr(gc, 'get_objects'):
            class G(object):
                def __cmp__(self, other):
                    return 0
                __hash__ = None # Silence Py3k warning
            g = G()
            orig_objects = len(gc.get_objects())
            for i in xrange(10):
                g==g
            new_objects = len(gc.get_objects())
            self.assertEqual(orig_objects, new_objects)

        class H(object):
            __slots__ = ['a', 'b']
            def __init__(self):
                self.a = 1
                self.b = 2
            def __del__(self_):
                self.assertEqual(self_.a, 1)
                self.assertEqual(self_.b, 2)
        with test_support.captured_output('stderr') as s:
            h = H()
            del h
        self.assertEqual(s.getvalue(), '')

        class X(object):
            __slots__ = "a"
        with self.assertRaises(AttributeError):
            del X().a

    def test_slots_special(self):
        # Testing __dict__ and __weakref__ in __slots__...
        class D(object):
            __slots__ = ["__dict__"]
        a = D()
        self.assertHasAttr(a, "__dict__")
        self.assertNotHasAttr(a, "__weakref__")
        a.foo = 42
        self.assertEqual(a.__dict__, {"foo": 42})

        class W(object):
            __slots__ = ["__weakref__"]
        a = W()
        self.assertHasAttr(a, "__weakref__")
        self.assertNotHasAttr(a, "__dict__")
        try:
            a.foo = 42
        except AttributeError:
            pass
        else:
            self.fail("shouldn't be allowed to set a.foo")

        class C1(W, D):
            __slots__ = []
        a = C1()
        self.assertHasAttr(a, "__dict__")
        self.assertHasAttr(a, "__weakref__")
        a.foo = 42
        self.assertEqual(a.__dict__, {"foo": 42})

        class C2(D, W):
            __slots__ = []
        a = C2()
        self.assertHasAttr(a, "__dict__")
        self.assertHasAttr(a, "__weakref__")
        a.foo = 42
        self.assertEqual(a.__dict__, {"foo": 42})

    def test_slots_descriptor(self):
        # Issue2115: slot descriptors did not correctly check
        # the type of the given object
        import abc
        class MyABC:
            __metaclass__ = abc.ABCMeta
            __slots__ = "a"

        class Unrelated(object):
            pass
        MyABC.register(Unrelated)

        u = Unrelated()
        self.assertIsInstance(u, MyABC)

        # This used to crash
        self.assertRaises(TypeError, MyABC.a.__set__, u, 3)

    def test_metaclass_cmp(self):
        # See bug 7491.
        class M(type):
            def __cmp__(self, other):
                return -1
        class X(object):
            __metaclass__ = M
        self.assertTrue(X < M)

    def test_dynamics(self):
        # Testing class attribute propagation...
        class D(object):
            pass
        class E(D):
            pass
        class F(D):
            pass
        D.foo = 1
        self.assertEqual(D.foo, 1)
        # Test that dynamic attributes are inherited
        self.assertEqual(E.foo, 1)
        self.assertEqual(F.foo, 1)
        # Test dynamic instances
        class C(object):
            pass
        a = C()
        self.assertNotHasAttr(a, "foobar")
        C.foobar = 2
        self.assertEqual(a.foobar, 2)
        C.method = lambda self: 42
        self.assertEqual(a.method(), 42)
        C.__repr__ = lambda self: "C()"
        self.assertEqual(repr(a), "C()")
        C.__int__ = lambda self: 100
        self.assertEqual(int(a), 100)
        self.assertEqual(a.foobar, 2)
        self.assertNotHasAttr(a, "spam")
        def mygetattr(self, name):
            if name == "spam":
                return "spam"
            raise AttributeError
        C.__getattr__ = mygetattr
        self.assertEqual(a.spam, "spam")
        a.new = 12
        self.assertEqual(a.new, 12)
        def mysetattr(self, name, value):
            if name == "spam":
                raise AttributeError
            return object.__setattr__(self, name, value)
        C.__setattr__ = mysetattr
        try:
            a.spam = "not spam"
        except AttributeError:
            pass
        else:
            self.fail("expected AttributeError")
        self.assertEqual(a.spam, "spam")
        class D(C):
            pass
        d = D()
        d.foo = 1
        self.assertEqual(d.foo, 1)

        # Test handling of int*seq and seq*int
        class I(int):
            pass
        self.assertEqual("a"*I(2), "aa")
        self.assertEqual(I(2)*"a", "aa")
        self.assertEqual(2*I(3), 6)
        self.assertEqual(I(3)*2, 6)
        self.assertEqual(I(3)*I(2), 6)

        # Test handling of long*seq and seq*long
        class L(long):
            pass
        self.assertEqual("a"*L(2L), "aa")
        self.assertEqual(L(2L)*"a", "aa")
        self.assertEqual(2*L(3), 6)
        self.assertEqual(L(3)*2, 6)
        self.assertEqual(L(3)*L(2), 6)

        # Test comparison of classes with dynamic metaclasses
        class dynamicmetaclass(type):
            pass
        class someclass:
            __metaclass__ = dynamicmetaclass
        self.assertNotEqual(someclass, object)

    def test_errors(self):
        # Testing errors...
        try:
            class C(list, dict):
                pass
        except TypeError:
            pass
        else:
            self.fail("inheritance from both list and dict should be illegal")

        try:
            class C(object, None):
                pass
        except TypeError:
            pass
        else:
            self.fail("inheritance from non-type should be illegal")
        class Classic:
            pass

        try:
            class C(type(len)):
                pass
        except TypeError:
            pass
        else:
            self.fail("inheritance from CFunction should be illegal")

        try:
            class C(object):
                __slots__ = 1
        except TypeError:
            pass
        else:
            self.fail("__slots__ = 1 should be illegal")

        try:
            class C(object):
                __slots__ = [1]
        except TypeError:
            pass
        else:
            self.fail("__slots__ = [1] should be illegal")

        class M1(type):
            pass
        class M2(type):
            pass
        class A1(object):
            __metaclass__ = M1
        class A2(object):
            __metaclass__ = M2
        try:
            class B(A1, A2):
                pass
        except TypeError:
            pass
        else:
            self.fail("finding the most derived metaclass should have failed")

    def test_classmethods(self):
        # Testing class methods...
        class C(object):
            def foo(*a): return a
            goo = classmethod(foo)
        c = C()
        self.assertEqual(C.goo(1), (C, 1))
        self.assertEqual(c.goo(1), (C, 1))
        self.assertEqual(c.foo(1), (c, 1))
        class D(C):
            pass
        d = D()
        self.assertEqual(D.goo(1), (D, 1))
        self.assertEqual(d.goo(1), (D, 1))
        self.assertEqual(d.foo(1), (d, 1))
        self.assertEqual(D.foo(d, 1), (d, 1))
        # Test for a specific crash (SF bug 528132)
        def f(cls, arg): return (cls, arg)
        ff = classmethod(f)
        self.assertEqual(ff.__get__(0, int)(42), (int, 42))
        self.assertEqual(ff.__get__(0)(42), (int, 42))

        # Test super() with classmethods (SF bug 535444)
        self.assertEqual(C.goo.im_self, C)
        self.assertEqual(D.goo.im_self, D)
        self.assertEqual(super(D,D).goo.im_self, D)
        self.assertEqual(super(D,d).goo.im_self, D)
        self.assertEqual(super(D,D).goo(), (D,))
        self.assertEqual(super(D,d).goo(), (D,))

        # Verify that a non-callable will raise
        meth = classmethod(1).__get__(1)
        self.assertRaises(TypeError, meth)

        # Verify that classmethod() doesn't allow keyword args
        try:
            classmethod(f, kw=1)
        except TypeError:
            pass
        else:
            self.fail("classmethod shouldn't accept keyword args")

    @test_support.impl_detail("the module 'xxsubtype' is internal")
    def test_classmethods_in_c(self):
        # Testing C-based class methods...
        import xxsubtype as spam
        a = (1, 2, 3)
        d = {'abc': 123}
        x, a1, d1 = spam.spamlist.classmeth(*a, **d)
        self.assertEqual(x, spam.spamlist)
        self.assertEqual(a, a1)
        self.assertEqual(d, d1)
        x, a1, d1 = spam.spamlist().classmeth(*a, **d)
        self.assertEqual(x, spam.spamlist)
        self.assertEqual(a, a1)
        self.assertEqual(d, d1)
        spam_cm = spam.spamlist.__dict__['classmeth']
        x2, a2, d2 = spam_cm(spam.spamlist, *a, **d)
        self.assertEqual(x2, spam.spamlist)
        self.assertEqual(a2, a1)
        self.assertEqual(d2, d1)
        class SubSpam(spam.spamlist): pass
        x2, a2, d2 = spam_cm(SubSpam, *a, **d)
        self.assertEqual(x2, SubSpam)
        self.assertEqual(a2, a1)
        self.assertEqual(d2, d1)
        with self.assertRaises(TypeError):
            spam_cm()
        with self.assertRaises(TypeError):
            spam_cm(spam.spamlist())
        with self.assertRaises(TypeError):
            spam_cm(list)

    def test_staticmethods(self):
        # Testing static methods...
        class C(object):
            def foo(*a): return a
            goo = staticmethod(foo)
        c = C()
        self.assertEqual(C.goo(1), (1,))
        self.assertEqual(c.goo(1), (1,))
        self.assertEqual(c.foo(1), (c, 1,))
        class D(C):
            pass
        d = D()
        self.assertEqual(D.goo(1), (1,))
        self.assertEqual(d.goo(1), (1,))
        self.assertEqual(d.foo(1), (d, 1))
        self.assertEqual(D.foo(d, 1), (d, 1))

    @test_support.impl_detail("the module 'xxsubtype' is internal")
    def test_staticmethods_in_c(self):
        # Testing C-based static methods...
        import xxsubtype as spam
        a = (1, 2, 3)
        d = {"abc": 123}
        x, a1, d1 = spam.spamlist.staticmeth(*a, **d)
        self.assertEqual(x, None)
        self.assertEqual(a, a1)
        self.assertEqual(d, d1)
        x, a1, d2 = spam.spamlist().staticmeth(*a, **d)
        self.assertEqual(x, None)
        self.assertEqual(a, a1)
        self.assertEqual(d, d1)

    def test_classic(self):
        # Testing classic classes...
        class C:
            def foo(*a): return a
            goo = classmethod(foo)
        c = C()
        self.assertEqual(C.goo(1), (C, 1))
        self.assertEqual(c.goo(1), (C, 1))
        self.assertEqual(c.foo(1), (c, 1))
        class D(C):
            pass
        d = D()
        self.assertEqual(D.goo(1), (D, 1))
        self.assertEqual(d.goo(1), (D, 1))
        self.assertEqual(d.foo(1), (d, 1))
        self.assertEqual(D.foo(d, 1), (d, 1))
        class E: # *not* subclassing from C
            foo = C.foo
        self.assertEqual(E().foo, C.foo) # i.e., unbound
        self.assertTrue(repr(C.foo.__get__(C())).startswith("<bound method "))

    def test_compattr(self):
        # Testing computed attributes...
        class C(object):
            class computed_attribute(object):
                def __init__(self, get, set=None, delete=None):
                    self.__get = get
                    self.__set = set
                    self.__delete = delete
                def __get__(self, obj, type=None):
                    return self.__get(obj)
                def __set__(self, obj, value):
                    return self.__set(obj, value)
                def __delete__(self, obj):
                    return self.__delete(obj)
            def __init__(self):
                self.__x = 0
            def __get_x(self):
                x = self.__x
                self.__x = x+1
                return x
            def __set_x(self, x):
                self.__x = x
            def __delete_x(self):
                del self.__x
            x = computed_attribute(__get_x, __set_x, __delete_x)
        a = C()
        self.assertEqual(a.x, 0)
        self.assertEqual(a.x, 1)
        a.x = 10
        self.assertEqual(a.x, 10)
        self.assertEqual(a.x, 11)
        del a.x
        self.assertNotHasAttr(a, 'x')

    def test_newslots(self):
        # Testing __new__ slot override...
        class C(list):
            def __new__(cls):
                self = list.__new__(cls)
                self.foo = 1
                return self
            def __init__(self):
                self.foo = self.foo + 2
        a = C()
        self.assertEqual(a.foo, 3)
        self.assertEqual(a.__class__, C)
        class D(C):
            pass
        b = D()
        self.assertEqual(b.foo, 3)
        self.assertEqual(b.__class__, D)

    def test_altmro(self):
        # Testing mro() and overriding it...
        class A(object):
            def f(self): return "A"
        class B(A):
            pass
        class C(A):
            def f(self): return "C"
        class D(B, C):
            pass
        self.assertEqual(D.mro(), [D, B, C, A, object])
        self.assertEqual(D.__mro__, (D, B, C, A, object))
        self.assertEqual(D().f(), "C")

        class PerverseMetaType(type):
            def mro(cls):
                L = type.mro(cls)
                L.reverse()
                return L
        class X(D,B,C,A):
            __metaclass__ = PerverseMetaType
        self.assertEqual(X.__mro__, (object, A, C, B, D, X))
        self.assertEqual(X().f(), "A")

        try:
            class X(object):
                class __metaclass__(type):
                    def mro(self):
                        return [self, dict, object]
            # In CPython, the class creation above already raises
            # TypeError, as a protection against the fact that
            # instances of X would segfault it.  In other Python
            # implementations it would be ok to let the class X
            # be created, but instead get a clean TypeError on the
            # __setitem__ below.
            x = object.__new__(X)
            x[5] = 6
        except TypeError:
            pass
        else:
            self.fail("devious mro() return not caught")

        try:
            class X(object):
                class __metaclass__(type):
                    def mro(self):
                        return [1]
        except TypeError:
            pass
        else:
            self.fail("non-class mro() return not caught")

        try:
            class X(object):
                class __metaclass__(type):
                    def mro(self):
                        return 1
        except TypeError:
            pass
        else:
            self.fail("non-sequence mro() return not caught")

    def test_overloading(self):
        # Testing operator overloading...

        class B(object):
            "Intermediate class because object doesn't have a __setattr__"

        class C(B):
            def __getattr__(self, name):
                if name == "foo":
                    return ("getattr", name)
                else:
                    raise AttributeError
            def __setattr__(self, name, value):
                if name == "foo":
                    self.setattr = (name, value)
                else:
                    return B.__setattr__(self, name, value)
            def __delattr__(self, name):
                if name == "foo":
                    self.delattr = name
                else:
                    return B.__delattr__(self, name)

            def __getitem__(self, key):
                return ("getitem", key)
            def __setitem__(self, key, value):
                self.setitem = (key, value)
            def __delitem__(self, key):
                self.delitem = key

            def __getslice__(self, i, j):
                return ("getslice", i, j)
            def __setslice__(self, i, j, value):
                self.setslice = (i, j, value)
            def __delslice__(self, i, j):
                self.delslice = (i, j)

        a = C()
        self.assertEqual(a.foo, ("getattr", "foo"))
        a.foo = 12
        self.assertEqual(a.setattr, ("foo", 12))
        del a.foo
        self.assertEqual(a.delattr, "foo")

        self.assertEqual(a[12], ("getitem", 12))
        a[12] = 21
        self.assertEqual(a.setitem, (12, 21))
        del a[12]
        self.assertEqual(a.delitem, 12)

        self.assertEqual(a[0:10], ("getslice", 0, 10))
        a[0:10] = "foo"
        self.assertEqual(a.setslice, (0, 10, "foo"))
        del a[0:10]
        self.assertEqual(a.delslice, (0, 10))

    def test_methods(self):
        # Testing methods...
        class C(object):
            def __init__(self, x):
                self.x = x
            def foo(self):
                return self.x
        c1 = C(1)
        self.assertEqual(c1.foo(), 1)
        class D(C):
            boo = C.foo
            goo = c1.foo
        d2 = D(2)
        self.assertEqual(d2.foo(), 2)
        self.assertEqual(d2.boo(), 2)
        self.assertEqual(d2.goo(), 1)
        class E(object):
            foo = C.foo
        self.assertEqual(E().foo, C.foo) # i.e., unbound
        self.assertTrue(repr(C.foo.__get__(C(1))).startswith("<bound method "))

    def test_special_method_lookup(self):
        # The lookup of special methods bypasses __getattr__ and
        # __getattribute__, but they still can be descriptors.

        def run_context(manager):
            with manager:
                pass
        def iden(self):
            return self
        def hello(self):
            return "hello"
        def empty_seq(self):
            return []
        def zero(self):
            return 0
        def complex_num(self):
            return 1j
        def stop(self):
            raise StopIteration
        def return_true(self, thing=None):
            return True
        def do_isinstance(obj):
            return isinstance(int, obj)
        def do_issubclass(obj):
            return issubclass(int, obj)
        def swallow(*args):
            pass
        def do_dict_missing(checker):
            class DictSub(checker.__class__, dict):
                pass
            self.assertEqual(DictSub()["hi"], 4)
        def some_number(self_, key):
            self.assertEqual(key, "hi")
            return 4
        def format_impl(self, spec):
            return "hello"

        # It would be nice to have every special method tested here, but I'm
        # only listing the ones I can remember outside of typeobject.c, since it
        # does it right.
        specials = [
            ("__unicode__", unicode, hello, set(), {}),
            ("__reversed__", reversed, empty_seq, set(), {}),
            ("__length_hint__", list, zero, set(),
             {"__iter__" : iden, "next" : stop}),
            ("__sizeof__", sys.getsizeof, zero, set(), {}),
            ("__instancecheck__", do_isinstance, return_true, set(), {}),
            ("__missing__", do_dict_missing, some_number,
             set(("__class__",)), {}),
            ("__subclasscheck__", do_issubclass, return_true,
             set(("__bases__",)), {}),
            ("__enter__", run_context, iden, set(), {"__exit__" : swallow}),
            ("__exit__", run_context, swallow, set(), {"__enter__" : iden}),
            ("__complex__", complex, complex_num, set(), {}),
            ("__format__", format, format_impl, set(), {}),
            ("__dir__", dir, empty_seq, set(), {}),
            ]

        class Checker(object):
            def __getattr__(self, attr, test=self):
                test.fail("__getattr__ called with {0}".format(attr))
            def __getattribute__(self, attr, test=self):
                if attr not in ok:
                    test.fail("__getattribute__ called with {0}".format(attr))
                return object.__getattribute__(self, attr)
        class SpecialDescr(object):
            def __init__(self, impl):
                self.impl = impl
            def __get__(self, obj, owner):
                record.append(1)
                return self.impl.__get__(obj, owner)
        class MyException(Exception):
            pass
        class ErrDescr(object):
            def __get__(self, obj, owner):
                raise MyException

        for name, runner, meth_impl, ok, env in specials:
            class X(Checker):
                pass
            for attr, obj in env.iteritems():
                setattr(X, attr, obj)
            setattr(X, name, meth_impl)
            runner(X())

            record = []
            class X(Checker):
                pass
            for attr, obj in env.iteritems():
                setattr(X, attr, obj)
            setattr(X, name, SpecialDescr(meth_impl))
            runner(X())
            self.assertEqual(record, [1], name)

            class X(Checker):
                pass
            for attr, obj in env.iteritems():
                setattr(X, attr, obj)
            setattr(X, name, ErrDescr())
            try:
                runner(X())
            except MyException:
                pass
            else:
                self.fail("{0!r} didn't raise".format(name))

    def test_specials(self):
        # Testing special operators...
        # Test operators like __hash__ for which a built-in default exists

        # Test the default behavior for static classes
        class C(object):
            def __getitem__(self, i):
                if 0 <= i < 10: return i
                raise IndexError
        c1 = C()
        c2 = C()
        self.assertFalse(not c1)
        self.assertNotEqual(id(c1), id(c2))
        hash(c1)
        hash(c2)
        self.assertEqual(cmp(c1, c2), cmp(id(c1), id(c2)))
        self.assertEqual(c1, c1)
        self.assertTrue(c1 != c2)
        self.assertFalse(c1 != c1)
        self.assertFalse(c1 == c2)
        # Note that the module name appears in str/repr, and that varies
        # depending on whether this test is run standalone or from a framework.
        self.assertGreaterEqual(str(c1).find('C object at '), 0)
        self.assertEqual(str(c1), repr(c1))
        self.assertNotIn(-1, c1)
        for i in range(10):
            self.assertIn(i, c1)
        self.assertNotIn(10, c1)
        # Test the default behavior for dynamic classes
        class D(object):
            def __getitem__(self, i):
                if 0 <= i < 10: return i
                raise IndexError
        d1 = D()
        d2 = D()
        self.assertFalse(not d1)
        self.assertNotEqual(id(d1), id(d2))
        hash(d1)
        hash(d2)
        self.assertEqual(cmp(d1, d2), cmp(id(d1), id(d2)))
        self.assertEqual(d1, d1)
        self.assertNotEqual(d1, d2)
        self.assertFalse(d1 != d1)
        self.assertFalse(d1 == d2)
        # Note that the module name appears in str/repr, and that varies
        # depending on whether this test is run standalone or from a framework.
        self.assertGreaterEqual(str(d1).find('D object at '), 0)
        self.assertEqual(str(d1), repr(d1))
        self.assertNotIn(-1, d1)
        for i in range(10):
            self.assertIn(i, d1)
        self.assertNotIn(10, d1)
        # Test overridden behavior for static classes
        class Proxy(object):
            def __init__(self, x):
                self.x = x
            def __nonzero__(self):
                return not not self.x
            def __hash__(self):
                return hash(self.x)
            def __eq__(self, other):
                return self.x == other
            def __ne__(self, other):
                return self.x != other
            def __cmp__(self, other):
                return cmp(self.x, other.x)
            def __str__(self):
                return "Proxy:%s" % self.x
            def __repr__(self):
                return "Proxy(%r)" % self.x
            def __contains__(self, value):
                return value in self.x
        p0 = Proxy(0)
        p1 = Proxy(1)
        p_1 = Proxy(-1)
        self.assertFalse(p0)
        self.assertFalse(not p1)
        self.assertEqual(hash(p0), hash(0))
        self.assertEqual(p0, p0)
        self.assertNotEqual(p0, p1)
        self.assertFalse(p0 != p0)
        self.assertEqual(not p0, p1)
        self.assertEqual(cmp(p0, p1), -1)
        self.assertEqual(cmp(p0, p0), 0)
        self.assertEqual(cmp(p0, p_1), 1)
        self.assertEqual(str(p0), "Proxy:0")
        self.assertEqual(repr(p0), "Proxy(0)")
        p10 = Proxy(range(10))
        self.assertNotIn(-1, p10)
        for i in range(10):
            self.assertIn(i, p10)
        self.assertNotIn(10, p10)
        # Test overridden behavior for dynamic classes
        class DProxy(object):
            def __init__(self, x):
                self.x = x
            def __nonzero__(self):
                return not not self.x
            def __hash__(self):
                return hash(self.x)
            def __eq__(self, other):
                return self.x == other
            def __ne__(self, other):
                return self.x != other
            def __cmp__(self, other):
                return cmp(self.x, other.x)
            def __str__(self):
                return "DProxy:%s" % self.x
            def __repr__(self):
                return "DProxy(%r)" % self.x
            def __contains__(self, value):
                return value in self.x
        p0 = DProxy(0)
        p1 = DProxy(1)
        p_1 = DProxy(-1)
        self.assertFalse(p0)
        self.assertFalse(not p1)
        self.assertEqual(hash(p0), hash(0))
        self.assertEqual(p0, p0)
        self.assertNotEqual(p0, p1)
        self.assertNotEqual(not p0, p0)
        self.assertEqual(not p0, p1)
        self.assertEqual(cmp(p0, p1), -1)
        self.assertEqual(cmp(p0, p0), 0)
        self.assertEqual(cmp(p0, p_1), 1)
        self.assertEqual(str(p0), "DProxy:0")
        self.assertEqual(repr(p0), "DProxy(0)")
        p10 = DProxy(range(10))
        self.assertNotIn(-1, p10)
        for i in range(10):
            self.assertIn(i, p10)
        self.assertNotIn(10, p10)

        # Safety test for __cmp__
        def unsafecmp(a, b):
            if not hasattr(a, '__cmp__'):
                return   # some types don't have a __cmp__ any more (so the
                         # test doesn't make sense any more), or maybe they
                         # never had a __cmp__ at all, e.g. in PyPy
            try:
                a.__class__.__cmp__(a, b)
            except TypeError:
                pass
            else:
                self.fail("shouldn't allow %s.__cmp__(%r, %r)" % (
                    a.__class__, a, b))

        unsafecmp(u"123", "123")
        unsafecmp("123", u"123")
        unsafecmp(1, 1.0)
        unsafecmp(1.0, 1)
        unsafecmp(1, 1L)
        unsafecmp(1L, 1)

    @test_support.impl_detail("custom logic for printing to real file objects")
    def test_recursions_1(self):
        # Testing recursion checks ...
        class Letter(str):
            def __new__(cls, letter):
                if letter == 'EPS':
                    return str.__new__(cls)
                return str.__new__(cls, letter)
            def __str__(self):
                if not self:
                    return 'EPS'
                return self
        # sys.stdout needs to be the original to trigger the recursion bug
        test_stdout = sys.stdout
        sys.stdout = test_support.get_original_stdout()
        try:
            # nothing should actually be printed, this should raise an exception
            print Letter('w')
        except RuntimeError:
            pass
        else:
            self.fail("expected a RuntimeError for print recursion")
        finally:
            sys.stdout = test_stdout

    def test_recursions_2(self):
        # Bug #1202533.
        class A(object):
            pass
        A.__mul__ = types.MethodType(lambda self, x: self * x, None, A)
        try:
            A()*2
        except RuntimeError:
            pass
        else:
            self.fail("expected a RuntimeError")

    def test_weakrefs(self):
        # Testing weak references...
        import weakref
        class C(object):
            pass
        c = C()
        r = weakref.ref(c)
        self.assertEqual(r(), c)
        del c
        test_support.gc_collect()
        self.assertEqual(r(), None)
        del r
        class NoWeak(object):
            __slots__ = ['foo']
        no = NoWeak()
        try:
            weakref.ref(no)
        except TypeError, msg:
            self.assertIn("weak reference", str(msg))
        else:
            self.fail("weakref.ref(no) should be illegal")
        class Weak(object):
            __slots__ = ['foo', '__weakref__']
        yes = Weak()
        r = weakref.ref(yes)
        self.assertEqual(r(), yes)
        del yes
        test_support.gc_collect()
        self.assertEqual(r(), None)
        del r

    def test_properties(self):
        # Testing property...
        class C(object):
            def getx(self):
                return self.__x
            def setx(self, value):
                self.__x = value
            def delx(self):
                del self.__x
            x = property(getx, setx, delx, doc="I'm the x property.")
        a = C()
        self.assertNotHasAttr(a, "x")
        a.x = 42
        self.assertEqual(a._C__x, 42)
        self.assertEqual(a.x, 42)
        del a.x
        self.assertNotHasAttr(a, "x")
        self.assertNotHasAttr(a, "_C__x")
        C.x.__set__(a, 100)
        self.assertEqual(C.x.__get__(a), 100)
        C.x.__delete__(a)
        self.assertNotHasAttr(a, "x")

        raw = C.__dict__['x']
        self.assertIsInstance(raw, property)

        attrs = dir(raw)
        self.assertIn("__doc__", attrs)
        self.assertIn("fget", attrs)
        self.assertIn("fset", attrs)
        self.assertIn("fdel", attrs)

        self.assertEqual(raw.__doc__, "I'm the x property.")
        self.assertIs(raw.fget, C.__dict__['getx'])
        self.assertIs(raw.fset, C.__dict__['setx'])
        self.assertIs(raw.fdel, C.__dict__['delx'])

        for attr in "__doc__", "fget", "fset", "fdel":
            try:
                setattr(raw, attr, 42)
            except TypeError, msg:
                if str(msg).find('readonly') < 0:
                    self.fail("when setting readonly attr %r on a property, "
                                     "got unexpected TypeError msg %r" % (attr, str(msg)))
            else:
                self.fail("expected TypeError from trying to set readonly %r "
                                 "attr on a property" % attr)

        class D(object):
            __getitem__ = property(lambda s: 1.0/0.0)

        d = D()
        try:
            for i in d:
                str(i)
        except ZeroDivisionError:
            pass
        else:
            self.fail("expected ZeroDivisionError from bad property")

    @unittest.skipIf(sys.flags.optimize >= 2,
                     "Docstrings are omitted with -O2 and above")
    def test_properties_doc_attrib(self):
        class E(object):
            def getter(self):
                "getter method"
                return 0
            def setter(self_, value):
                "setter method"
                pass
            prop = property(getter)
            self.assertEqual(prop.__doc__, "getter method")
            prop2 = property(fset=setter)
            self.assertEqual(prop2.__doc__, None)

    @test_support.cpython_only
    def test_testcapi_no_segfault(self):
        # this segfaulted in 2.5b2
        try:
            import _testcapi
        except ImportError:
            pass
        else:
            class X(object):
                p = property(_testcapi.test_with_docstring)

    def test_properties_plus(self):
        class C(object):
            foo = property(doc="hello")
            @foo.getter
            def foo(self):
                return self._foo
            @foo.setter
            def foo(self, value):
                self._foo = abs(value)
            @foo.deleter
            def foo(self):
                del self._foo
        c = C()
        self.assertEqual(C.foo.__doc__, "hello")
        self.assertNotHasAttr(c, "foo")
        c.foo = -42
        self.assertHasAttr(c, '_foo')
        self.assertEqual(c._foo, 42)
        self.assertEqual(c.foo, 42)
        del c.foo
        self.assertNotHasAttr(c, '_foo')
        self.assertNotHasAttr(c, "foo")

        class D(C):
            @C.foo.deleter
            def foo(self):
                try:
                    del self._foo
                except AttributeError:
                    pass
        d = D()
        d.foo = 24
        self.assertEqual(d.foo, 24)
        del d.foo
        del d.foo

        class E(object):
            @property
            def foo(self):
                return self._foo
            @foo.setter
            def foo(self, value):
                raise RuntimeError
            @foo.setter
            def foo(self, value):
                self._foo = abs(value)
            @foo.deleter
            def foo(self, value=None):
                del self._foo

        e = E()
        e.foo = -42
        self.assertEqual(e.foo, 42)
        del e.foo

        class F(E):
            @E.foo.deleter
            def foo(self):
                del self._foo
            @foo.setter
            def foo(self, value):
                self._foo = max(0, value)
        f = F()
        f.foo = -10
        self.assertEqual(f.foo, 0)
        del f.foo

    def test_dict_constructors(self):
        # Testing dict constructor ...
        d = dict()
        self.assertEqual(d, {})
        d = dict({})
        self.assertEqual(d, {})
        d = dict({1: 2, 'a': 'b'})
        self.assertEqual(d, {1: 2, 'a': 'b'})
        self.assertEqual(d, dict(d.items()))
        self.assertEqual(d, dict(d.iteritems()))
        d = dict({'one':1, 'two':2})
        self.assertEqual(d, dict(one=1, two=2))
        self.assertEqual(d, dict(**d))
        self.assertEqual(d, dict({"one": 1}, two=2))
        self.assertEqual(d, dict([("two", 2)], one=1))
        self.assertEqual(d, dict([("one", 100), ("two", 200)], **d))
        self.assertEqual(d, dict(**d))

        for badarg in 0, 0L, 0j, "0", [0], (0,):
            try:
                dict(badarg)
            except TypeError:
                pass
            except ValueError:
                if badarg == "0":
                    # It's a sequence, and its elements are also sequences (gotta
                    # love strings <wink>), but they aren't of length 2, so this
                    # one seemed better as a ValueError than a TypeError.
                    pass
                else:
                    self.fail("no TypeError from dict(%r)" % badarg)
            else:
                self.fail("no TypeError from dict(%r)" % badarg)

        try:
            dict({}, {})
        except TypeError:
            pass
        else:
            self.fail("no TypeError from dict({}, {})")

        class Mapping:
            # Lacks a .keys() method; will be added later.
            dict = {1:2, 3:4, 'a':1j}

        try:
            dict(Mapping())
        except TypeError:
            pass
        else:
            self.fail("no TypeError from dict(incomplete mapping)")

        Mapping.keys = lambda self: self.dict.keys()
        Mapping.__getitem__ = lambda self, i: self.dict[i]
        d = dict(Mapping())
        self.assertEqual(d, Mapping.dict)

        # Init from sequence of iterable objects, each producing a 2-sequence.
        class AddressBookEntry:
            def __init__(self, first, last):
                self.first = first
                self.last = last
            def __iter__(self):
                return iter([self.first, self.last])

        d = dict([AddressBookEntry('Tim', 'Warsaw'),
                  AddressBookEntry('Barry', 'Peters'),
                  AddressBookEntry('Tim', 'Peters'),
                  AddressBookEntry('Barry', 'Warsaw')])
        self.assertEqual(d, {'Barry': 'Warsaw', 'Tim': 'Peters'})

        d = dict(zip(range(4), range(1, 5)))
        self.assertEqual(d, dict([(i, i+1) for i in range(4)]))

        # Bad sequence lengths.
        for bad in [('tooshort',)], [('too', 'long', 'by 1')]:
            try:
                dict(bad)
            except ValueError:
                pass
            else:
                self.fail("no ValueError from dict(%r)" % bad)

    def test_dir(self):
        # Testing dir() ...
        junk = 12
        self.assertEqual(dir(), ['junk', 'self'])
        del junk

        # Just make sure these don't blow up!
        for arg in 2, 2L, 2j, 2e0, [2], "2", u"2", (2,), {2:2}, type, self.test_dir:
            dir(arg)

        # Try classic classes.
        class C:
            Cdata = 1
            def Cmethod(self): pass

        cstuff = ['Cdata', 'Cmethod', '__doc__', '__module__']
        self.assertEqual(dir(C), cstuff)
        self.assertIn('im_self', dir(C.Cmethod))

        c = C()  # c.__doc__ is an odd thing to see here; ditto c.__module__.
        self.assertEqual(dir(c), cstuff)

        c.cdata = 2
        c.cmethod = lambda self: 0
        self.assertEqual(dir(c), cstuff + ['cdata', 'cmethod'])
        self.assertIn('im_self', dir(c.Cmethod))

        class A(C):
            Adata = 1
            def Amethod(self): pass

        astuff = ['Adata', 'Amethod'] + cstuff
        self.assertEqual(dir(A), astuff)
        self.assertIn('im_self', dir(A.Amethod))
        a = A()
        self.assertEqual(dir(a), astuff)
        self.assertIn('im_self', dir(a.Amethod))
        a.adata = 42
        a.amethod = lambda self: 3
        self.assertEqual(dir(a), astuff + ['adata', 'amethod'])

        # The same, but with new-style classes.  Since these have object as a
        # base class, a lot more gets sucked in.
        def interesting(strings):
            return [s for s in strings if not s.startswith('_')]

        class C(object):
            Cdata = 1
            def Cmethod(self): pass

        cstuff = ['Cdata', 'Cmethod']
        self.assertEqual(interesting(dir(C)), cstuff)

        c = C()
        self.assertEqual(interesting(dir(c)), cstuff)
        self.assertIn('im_self', dir(C.Cmethod))

        c.cdata = 2
        c.cmethod = lambda self: 0
        self.assertEqual(interesting(dir(c)), cstuff + ['cdata', 'cmethod'])
        self.assertIn('im_self', dir(c.Cmethod))

        class A(C):
            Adata = 1
            def Amethod(self): pass

        astuff = ['Adata', 'Amethod'] + cstuff
        self.assertEqual(interesting(dir(A)), astuff)
        self.assertIn('im_self', dir(A.Amethod))
        a = A()
        self.assertEqual(interesting(dir(a)), astuff)
        a.adata = 42
        a.amethod = lambda self: 3
        self.assertEqual(interesting(dir(a)), astuff + ['adata', 'amethod'])
        self.assertIn('im_self', dir(a.Amethod))

        # Try a module subclass.
        class M(type(sys)):
            pass
        minstance = M("m")
        minstance.b = 2
        minstance.a = 1
        names = [x for x in dir(minstance) if x not in ["__name__", "__doc__"]]
        self.assertEqual(names, ['a', 'b'])

        class M2(M):
            def getdict(self):
                return "Not a dict!"
            __dict__ = property(getdict)

        m2instance = M2("m2")
        m2instance.b = 2
        m2instance.a = 1
        self.assertEqual(m2instance.__dict__, "Not a dict!")
        try:
            dir(m2instance)
        except TypeError:
            pass

        # Two essentially featureless objects, just inheriting stuff from
        # object.
        self.assertEqual(dir(NotImplemented), dir(Ellipsis))
        if test_support.check_impl_detail():
            # None differs in PyPy: it has a __nonzero__
            self.assertEqual(dir(None), dir(Ellipsis))

        # Nasty test case for proxied objects
        class Wrapper(object):
            def __init__(self, obj):
                self.__obj = obj
            def __repr__(self):
                return "Wrapper(%s)" % repr(self.__obj)
            def __getitem__(self, key):
                return Wrapper(self.__obj[key])
            def __len__(self):
                return len(self.__obj)
            def __getattr__(self, name):
                return Wrapper(getattr(self.__obj, name))

        class C(object):
            def __getclass(self):
                return Wrapper(type(self))
            __class__ = property(__getclass)

        dir(C()) # This used to segfault

    def test_supers(self):
        # Testing super...

        class A(object):
            def meth(self, a):
                return "A(%r)" % a

        self.assertEqual(A().meth(1), "A(1)")

        class B(A):
            def __init__(self):
                self.__super = super(B, self)
            def meth(self, a):
                return "B(%r)" % a + self.__super.meth(a)

        self.assertEqual(B().meth(2), "B(2)A(2)")

        class C(A):
            def meth(self, a):
                return "C(%r)" % a + self.__super.meth(a)
        C._C__super = super(C)

        self.assertEqual(C().meth(3), "C(3)A(3)")

        class D(C, B):
            def meth(self, a):
                return "D(%r)" % a + super(D, self).meth(a)

        self.assertEqual(D().meth(4), "D(4)C(4)B(4)A(4)")

        # Test for subclassing super

        class mysuper(super):
            def __init__(self, *args):
                return super(mysuper, self).__init__(*args)

        class E(D):
            def meth(self, a):
                return "E(%r)" % a + mysuper(E, self).meth(a)

        self.assertEqual(E().meth(5), "E(5)D(5)C(5)B(5)A(5)")

        class F(E):
            def meth(self, a):
                s = self.__super # == mysuper(F, self)
                return "F(%r)[%s]" % (a, s.__class__.__name__) + s.meth(a)
        F._F__super = mysuper(F)

        self.assertEqual(F().meth(6), "F(6)[mysuper]E(6)D(6)C(6)B(6)A(6)")

        # Make sure certain errors are raised

        try:
            super(D, 42)
        except TypeError:
            pass
        else:
            self.fail("shouldn't allow super(D, 42)")

        try:
            super(D, C())
        except TypeError:
            pass
        else:
            self.fail("shouldn't allow super(D, C())")

        try:
            super(D).__get__(12)
        except TypeError:
            pass
        else:
            self.fail("shouldn't allow super(D).__get__(12)")

        try:
            super(D).__get__(C())
        except TypeError:
            pass
        else:
            self.fail("shouldn't allow super(D).__get__(C())")

        # Make sure data descriptors can be overridden and accessed via super
        # (new feature in Python 2.3)

        class DDbase(object):
            def getx(self): return 42
            x = property(getx)

        class DDsub(DDbase):
            def getx(self): return "hello"
            x = property(getx)

        dd = DDsub()
        self.assertEqual(dd.x, "hello")
        self.assertEqual(super(DDsub, dd).x, 42)

        # Ensure that super() lookup of descriptor from classmethod
        # works (SF ID# 743627)

        class Base(object):
            aProp = property(lambda self: "foo")

        class Sub(Base):
            @classmethod
            def test(klass):
                return super(Sub,klass).aProp

        self.assertEqual(Sub.test(), Base.aProp)

        # Verify that super() doesn't allow keyword args
        try:
            super(Base, kw=1)
        except TypeError:
            pass
        else:
            self.assertEqual("super shouldn't accept keyword args")

    def test_basic_inheritance(self):
        # Testing inheritance from basic types...

        class hexint(int):
            def __repr__(self):
                return hex(self)
            def __add__(self, other):
                return hexint(int.__add__(self, other))
            # (Note that overriding __radd__ doesn't work,
            # because the int type gets first dibs.)
        self.assertEqual(repr(hexint(7) + 9), "0x10")
        self.assertEqual(repr(hexint(1000) + 7), "0x3ef")
        a = hexint(12345)
        self.assertEqual(a, 12345)
        self.assertEqual(int(a), 12345)
        self.assertIs(int(a).__class__, int)
        self.assertEqual(hash(a), hash(12345))
        self.assertIs((+a).__class__, int)
        self.assertIs((a >> 0).__class__, int)
        self.assertIs((a << 0).__class__, int)
        self.assertIs((hexint(0) << 12).__class__, int)
        self.assertIs((hexint(0) >> 12).__class__, int)

        class octlong(long):
            __slots__ = []
            def __str__(self):
                s = oct(self)
                if s[-1] == 'L':
                    s = s[:-1]
                return s
            def __add__(self, other):
                return self.__class__(super(octlong, self).__add__(other))
            __radd__ = __add__
        self.assertEqual(str(octlong(3) + 5), "010")
        # (Note that overriding __radd__ here only seems to work
        # because the example uses a short int left argument.)
        self.assertEqual(str(5 + octlong(3000)), "05675")
        a = octlong(12345)
        self.assertEqual(a, 12345L)
        self.assertEqual(long(a), 12345L)
        self.assertEqual(hash(a), hash(12345L))
        self.assertIs(long(a).__class__, long)
        self.assertIs((+a).__class__, long)
        self.assertIs((-a).__class__, long)
        self.assertIs((-octlong(0)).__class__, long)
        self.assertIs((a >> 0).__class__, long)
        self.assertIs((a << 0).__class__, long)
        self.assertIs((a - 0).__class__, long)
        self.assertIs((a * 1).__class__, long)
        self.assertIs((a ** 1).__class__, long)
        self.assertIs((a // 1).__class__, long)
        self.assertIs((1 * a).__class__, long)
        self.assertIs((a | 0).__class__, long)
        self.assertIs((a ^ 0).__class__, long)
        self.assertIs((a & -1L).__class__, long)
        self.assertIs((octlong(0) << 12).__class__, long)
        self.assertIs((octlong(0) >> 12).__class__, long)
        self.assertIs(abs(octlong(0)).__class__, long)

        # Because octlong overrides __add__, we can't check the absence of +0
        # optimizations using octlong.
        class longclone(long):
            pass
        a = longclone(1)
        self.assertIs((a + 0).__class__, long)
        self.assertIs((0 + a).__class__, long)

        # Check that negative clones don't segfault
        a = longclone(-1)
        self.assertEqual(a.__dict__, {})
        self.assertEqual(long(a), -1)  # self.assertTrue PyNumber_Long() copies the sign bit

        class precfloat(float):
            __slots__ = ['prec']
            def __init__(self, value=0.0, prec=12):
                self.prec = int(prec)
            def __repr__(self):
                return "%.*g" % (self.prec, self)
        self.assertEqual(repr(precfloat(1.1)), "1.1")
        a = precfloat(12345)
        self.assertEqual(a, 12345.0)
        self.assertEqual(float(a), 12345.0)
        self.assertIs(float(a).__class__, float)
        self.assertEqual(hash(a), hash(12345.0))
        self.assertIs((+a).__class__, float)

        class madcomplex(complex):
            def __repr__(self):
                return "%.17gj%+.17g" % (self.imag, self.real)
        a = madcomplex(-3, 4)
        self.assertEqual(repr(a), "4j-3")
        base = complex(-3, 4)
        self.assertEqual(base.__class__, complex)
        self.assertEqual(a, base)
        self.assertEqual(complex(a), base)
        self.assertEqual(complex(a).__class__, complex)
        a = madcomplex(a)  # just trying another form of the constructor
        self.assertEqual(repr(a), "4j-3")
        self.assertEqual(a, base)
        self.assertEqual(complex(a), base)
        self.assertEqual(complex(a).__class__, complex)
        self.assertEqual(hash(a), hash(base))
        self.assertEqual((+a).__class__, complex)
        self.assertEqual((a + 0).__class__, complex)
        self.assertEqual(a + 0, base)
        self.assertEqual((a - 0).__class__, complex)
        self.assertEqual(a - 0, base)
        self.assertEqual((a * 1).__class__, complex)
        self.assertEqual(a * 1, base)
        self.assertEqual((a / 1).__class__, complex)
        self.assertEqual(a / 1, base)

        class madtuple(tuple):
            _rev = None
            def rev(self):
                if self._rev is not None:
                    return self._rev
                L = list(self)
                L.reverse()
                self._rev = self.__class__(L)
                return self._rev
        a = madtuple((1,2,3,4,5,6,7,8,9,0))
        self.assertEqual(a, (1,2,3,4,5,6,7,8,9,0))
        self.assertEqual(a.rev(), madtuple((0,9,8,7,6,5,4,3,2,1)))
        self.assertEqual(a.rev().rev(), madtuple((1,2,3,4,5,6,7,8,9,0)))
        for i in range(512):
            t = madtuple(range(i))
            u = t.rev()
            v = u.rev()
            self.assertEqual(v, t)
        a = madtuple((1,2,3,4,5))
        self.assertEqual(tuple(a), (1,2,3,4,5))
        self.assertIs(tuple(a).__class__, tuple)
        self.assertEqual(hash(a), hash((1,2,3,4,5)))
        self.assertIs(a[:].__class__, tuple)
        self.assertIs((a * 1).__class__, tuple)
        self.assertIs((a * 0).__class__, tuple)
        self.assertIs((a + ()).__class__, tuple)
        a = madtuple(())
        self.assertEqual(tuple(a), ())
        self.assertIs(tuple(a).__class__, tuple)
        self.assertIs((a + a).__class__, tuple)
        self.assertIs((a * 0).__class__, tuple)
        self.assertIs((a * 1).__class__, tuple)
        self.assertIs((a * 2).__class__, tuple)
        self.assertIs(a[:].__class__, tuple)

        class madstring(str):
            _rev = None
            def rev(self):
                if self._rev is not None:
                    return self._rev
                L = list(self)
                L.reverse()
                self._rev = self.__class__("".join(L))
                return self._rev
        s = madstring("abcdefghijklmnopqrstuvwxyz")
        self.assertEqual(s, "abcdefghijklmnopqrstuvwxyz")
        self.assertEqual(s.rev(), madstring("zyxwvutsrqponmlkjihgfedcba"))
        self.assertEqual(s.rev().rev(), madstring("abcdefghijklmnopqrstuvwxyz"))
        for i in range(256):
            s = madstring("".join(map(chr, range(i))))
            t = s.rev()
            u = t.rev()
            self.assertEqual(u, s)
        s = madstring("12345")
        self.assertEqual(str(s), "12345")
        self.assertIs(str(s).__class__, str)

        base = "\x00" * 5
        s = madstring(base)
        self.assertEqual(s, base)
        self.assertEqual(str(s), base)
        self.assertIs(str(s).__class__, str)
        self.assertEqual(hash(s), hash(base))
        self.assertEqual({s: 1}[base], 1)
        self.assertEqual({base: 1}[s], 1)
        self.assertIs((s + "").__class__, str)
        self.assertEqual(s + "", base)
        self.assertIs(("" + s).__class__, str)
        self.assertEqual("" + s, base)
        self.assertIs((s * 0).__class__, str)
        self.assertEqual(s * 0, "")
        self.assertIs((s * 1).__class__, str)
        self.assertEqual(s * 1, base)
        self.assertIs((s * 2).__class__, str)
        self.assertEqual(s * 2, base + base)
        self.assertIs(s[:].__class__, str)
        self.assertEqual(s[:], base)
        self.assertIs(s[0:0].__class__, str)
        self.assertEqual(s[0:0], "")
        self.assertIs(s.strip().__class__, str)
        self.assertEqual(s.strip(), base)
        self.assertIs(s.lstrip().__class__, str)
        self.assertEqual(s.lstrip(), base)
        self.assertIs(s.rstrip().__class__, str)
        self.assertEqual(s.rstrip(), base)
        identitytab = ''.join([chr(i) for i in range(256)])
        self.assertIs(s.translate(identitytab).__class__, str)
        self.assertEqual(s.translate(identitytab), base)
        self.assertIs(s.translate(identitytab, "x").__class__, str)
        self.assertEqual(s.translate(identitytab, "x"), base)
        self.assertEqual(s.translate(identitytab, "\x00"), "")
        self.assertIs(s.replace("x", "x").__class__, str)
        self.assertEqual(s.replace("x", "x"), base)
        self.assertIs(s.ljust(len(s)).__class__, str)
        self.assertEqual(s.ljust(len(s)), base)
        self.assertIs(s.rjust(len(s)).__class__, str)
        self.assertEqual(s.rjust(len(s)), base)
        self.assertIs(s.center(len(s)).__class__, str)
        self.assertEqual(s.center(len(s)), base)
        self.assertIs(s.lower().__class__, str)
        self.assertEqual(s.lower(), base)

        class madunicode(unicode):
            _rev = None
            def rev(self):
                if self._rev is not None:
                    return self._rev
                L = list(self)
                L.reverse()
                self._rev = self.__class__(u"".join(L))
                return self._rev
        u = madunicode("ABCDEF")
        self.assertEqual(u, u"ABCDEF")
        self.assertEqual(u.rev(), madunicode(u"FEDCBA"))
        self.assertEqual(u.rev().rev(), madunicode(u"ABCDEF"))
        base = u"12345"
        u = madunicode(base)
        self.assertEqual(unicode(u), base)
        self.assertIs(unicode(u).__class__, unicode)
        self.assertEqual(hash(u), hash(base))
        self.assertEqual({u: 1}[base], 1)
        self.assertEqual({base: 1}[u], 1)
        self.assertIs(u.strip().__class__, unicode)
        self.assertEqual(u.strip(), base)
        self.assertIs(u.lstrip().__class__, unicode)
        self.assertEqual(u.lstrip(), base)
        self.assertIs(u.rstrip().__class__, unicode)
        self.assertEqual(u.rstrip(), base)
        self.assertIs(u.replace(u"x", u"x").__class__, unicode)
        self.assertEqual(u.replace(u"x", u"x"), base)
        self.assertIs(u.replace(u"xy", u"xy").__class__, unicode)
        self.assertEqual(u.replace(u"xy", u"xy"), base)
        self.assertIs(u.center(len(u)).__class__, unicode)
        self.assertEqual(u.center(len(u)), base)
        self.assertIs(u.ljust(len(u)).__class__, unicode)
        self.assertEqual(u.ljust(len(u)), base)
        self.assertIs(u.rjust(len(u)).__class__, unicode)
        self.assertEqual(u.rjust(len(u)), base)
        self.assertIs(u.lower().__class__, unicode)
        self.assertEqual(u.lower(), base)
        self.assertIs(u.upper().__class__, unicode)
        self.assertEqual(u.upper(), base)
        self.assertIs(u.capitalize().__class__, unicode)
        self.assertEqual(u.capitalize(), base)
        self.assertIs(u.title().__class__, unicode)
        self.assertEqual(u.title(), base)
        self.assertIs((u + u"").__class__, unicode)
        self.assertEqual(u + u"", base)
        self.assertIs((u"" + u).__class__, unicode)
        self.assertEqual(u"" + u, base)
        self.assertIs((u * 0).__class__, unicode)
        self.assertEqual(u * 0, u"")
        self.assertIs((u * 1).__class__, unicode)
        self.assertEqual(u * 1, base)
        self.assertIs((u * 2).__class__, unicode)
        self.assertEqual(u * 2, base + base)
        self.assertIs(u[:].__class__, unicode)
        self.assertEqual(u[:], base)
        self.assertIs(u[0:0].__class__, unicode)
        self.assertEqual(u[0:0], u"")

        class sublist(list):
            pass
        a = sublist(range(5))
        self.assertEqual(a, range(5))
        a.append("hello")
        self.assertEqual(a, range(5) + ["hello"])
        a[5] = 5
        self.assertEqual(a, range(6))
        a.extend(range(6, 20))
        self.assertEqual(a, range(20))
        a[-5:] = []
        self.assertEqual(a, range(15))
        del a[10:15]
        self.assertEqual(len(a), 10)
        self.assertEqual(a, range(10))
        self.assertEqual(list(a), range(10))
        self.assertEqual(a[0], 0)
        self.assertEqual(a[9], 9)
        self.assertEqual(a[-10], 0)
        self.assertEqual(a[-1], 9)
        self.assertEqual(a[:5], range(5))

        class CountedInput(file):
            """Counts lines read by self.readline().

            self.lineno is the 0-based ordinal of the last line read, up to
            a maximum of one greater than the number of lines in the file.

            self.ateof is true if and only if the final "" line has been read,
            at which point self.lineno stops incrementing, and further calls
            to readline() continue to return "".
            """

            lineno = 0
            ateof = 0
            def readline(self):
                if self.ateof:
                    return ""
                s = file.readline(self)
                # Next line works too.
                # s = super(CountedInput, self).readline()
                self.lineno += 1
                if s == "":
                    self.ateof = 1
                return s

        f = file(name=test_support.TESTFN, mode='w')
        lines = ['a\n', 'b\n', 'c\n']
        try:
            f.writelines(lines)
            f.close()
            f = CountedInput(test_support.TESTFN)
            for (i, expected) in zip(range(1, 5) + [4], lines + 2 * [""]):
                got = f.readline()
                self.assertEqual(expected, got)
                self.assertEqual(f.lineno, i)
                self.assertEqual(f.ateof, (i > len(lines)))
            f.close()
        finally:
            try:
                f.close()
            except:
                pass
            test_support.unlink(test_support.TESTFN)

    def test_keywords(self):
        # Testing keyword args to basic type constructors ...
        self.assertEqual(int(x=1), 1)
        self.assertEqual(float(x=2), 2.0)
        self.assertEqual(long(x=3), 3L)
        self.assertEqual(complex(imag=42, real=666), complex(666, 42))
        self.assertEqual(str(object=500), '500')
        self.assertEqual(unicode(string='abc', errors='strict'), u'abc')
        self.assertEqual(tuple(sequence=range(3)), (0, 1, 2))
        self.assertEqual(list(sequence=(0, 1, 2)), range(3))
        # note: as of Python 2.3, dict() no longer has an "items" keyword arg

        for constructor in (int, float, long, complex, str, unicode,
                            tuple, list, file):
            try:
                constructor(bogus_keyword_arg=1)
            except TypeError:
                pass
            else:
                self.fail("expected TypeError from bogus keyword argument to %r"
                            % constructor)

    def test_str_subclass_as_dict_key(self):
        # Testing a str subclass used as dict key ..

        class cistr(str):
            """Sublcass of str that computes __eq__ case-insensitively.

            Also computes a hash code of the string in canonical form.
            """

            def __init__(self, value):
                self.canonical = value.lower()
                self.hashcode = hash(self.canonical)

            def __eq__(self, other):
                if not isinstance(other, cistr):
                    other = cistr(other)
                return self.canonical == other.canonical

            def __hash__(self):
                return self.hashcode

        self.assertEqual(cistr('ABC'), 'abc')
        self.assertEqual('aBc', cistr('ABC'))
        self.assertEqual(str(cistr('ABC')), 'ABC')

        d = {cistr('one'): 1, cistr('two'): 2, cistr('tHree'): 3}
        self.assertEqual(d[cistr('one')], 1)
        self.assertEqual(d[cistr('tWo')], 2)
        self.assertEqual(d[cistr('THrEE')], 3)
        self.assertIn(cistr('ONe'), d)
        self.assertEqual(d.get(cistr('thrEE')), 3)

    def test_classic_comparisons(self):
        # Testing classic comparisons...
        class classic:
            pass

        for base in (classic, int, object):
            class C(base):
                def __init__(self, value):
                    self.value = int(value)
                def __cmp__(self, other):
                    if isinstance(other, C):
                        return cmp(self.value, other.value)
                    if isinstance(other, int) or isinstance(other, long):
                        return cmp(self.value, other)
                    return NotImplemented
                __hash__ = None # Silence Py3k warning

            c1 = C(1)
            c2 = C(2)
            c3 = C(3)
            self.assertEqual(c1, 1)
            c = {1: c1, 2: c2, 3: c3}
            for x in 1, 2, 3:
                for y in 1, 2, 3:
                    self.assertEqual(cmp(c[x], c[y]), cmp(x, y),
                                     "x=%d, y=%d" % (x, y))
                    for op in "<", "<=", "==", "!=", ">", ">=":
                        self.assertEqual(eval("c[x] %s c[y]" % op),
                                         eval("x %s y" % op),
                                         "x=%d, y=%d" % (x, y))
                    self.assertEqual(cmp(c[x], y), cmp(x, y),
                                     "x=%d, y=%d" % (x, y))
                    self.assertEqual(cmp(x, c[y]), cmp(x, y),
                                     "x=%d, y=%d" % (x, y))

    def test_rich_comparisons(self):
        # Testing rich comparisons...
        class Z(complex):
            pass
        z = Z(1)
        self.assertEqual(z, 1+0j)
        self.assertEqual(1+0j, z)
        class ZZ(complex):
            def __eq__(self, other):
                try:
                    return abs(self - other) <= 1e-6
                except:
                    return NotImplemented
            __hash__ = None # Silence Py3k warning
        zz = ZZ(1.0000003)
        self.assertEqual(zz, 1+0j)
        self.assertEqual(1+0j, zz)

        class classic:
            pass
        for base in (classic, int, object, list):
            class C(base):
                def __init__(self, value):
                    self.value = int(value)
                def __cmp__(self_, other):
                    self.fail("shouldn't call __cmp__")
                __hash__ = None # Silence Py3k warning
                def __eq__(self, other):
                    if isinstance(other, C):
                        return self.value == other.value
                    if isinstance(other, int) or isinstance(other, long):
                        return self.value == other
                    return NotImplemented
                def __ne__(self, other):
                    if isinstance(other, C):
                        return self.value != other.value
                    if isinstance(other, int) or isinstance(other, long):
                        return self.value != other
                    return NotImplemented
                def __lt__(self, other):
                    if isinstance(other, C):
                        return self.value < other.value
                    if isinstance(other, int) or isinstance(other, long):
                        return self.value < other
                    return NotImplemented
                def __le__(self, other):
                    if isinstance(other, C):
                        return self.value <= other.value
                    if isinstance(other, int) or isinstance(other, long):
                        return self.value <= other
                    return NotImplemented
                def __gt__(self, other):
                    if isinstance(other, C):
                        return self.value > other.value
                    if isinstance(other, int) or isinstance(other, long):
                        return self.value > other
                    return NotImplemented
                def __ge__(self, other):
                    if isinstance(other, C):
                        return self.value >= other.value
                    if isinstance(other, int) or isinstance(other, long):
                        return self.value >= other
                    return NotImplemented
            c1 = C(1)
            c2 = C(2)
            c3 = C(3)
            self.assertEqual(c1, 1)
            c = {1: c1, 2: c2, 3: c3}
            for x in 1, 2, 3:
                for y in 1, 2, 3:
                    for op in "<", "<=", "==", "!=", ">", ">=":
                        self.assertEqual(eval("c[x] %s c[y]" % op),
                                         eval("x %s y" % op),
                                         "x=%d, y=%d" % (x, y))
                        self.assertEqual(eval("c[x] %s y" % op),
                                         eval("x %s y" % op),
                                         "x=%d, y=%d" % (x, y))
                        self.assertEqual(eval("x %s c[y]" % op),
                                         eval("x %s y" % op),
                                         "x=%d, y=%d" % (x, y))

    def test_coercions(self):
        # Testing coercions...
        class I(int): pass
        coerce(I(0), 0)
        coerce(0, I(0))
        class L(long): pass
        coerce(L(0), 0)
        coerce(L(0), 0L)
        coerce(0, L(0))
        coerce(0L, L(0))
        class F(float): pass
        coerce(F(0), 0)
        coerce(F(0), 0L)
        coerce(F(0), 0.)
        coerce(0, F(0))
        coerce(0L, F(0))
        coerce(0., F(0))
        class C(complex): pass
        coerce(C(0), 0)
        coerce(C(0), 0L)
        coerce(C(0), 0.)
        coerce(C(0), 0j)
        coerce(0, C(0))
        coerce(0L, C(0))
        coerce(0., C(0))
        coerce(0j, C(0))

    def test_descrdoc(self):
        # Testing descriptor doc strings...
        def check(descr, what):
            self.assertEqual(descr.__doc__, what)
        check(file.closed, "True if the file is closed") # getset descriptor
        check(file.name, "file name") # member descriptor

    def test_doc_descriptor(self):
        # Testing __doc__ descriptor...
        # SF bug 542984
        class DocDescr(object):
            def __get__(self, object, otype):
                if object:
                    object = object.__class__.__name__ + ' instance'
                if otype:
                    otype = otype.__name__
                return 'object=%s; type=%s' % (object, otype)
        class OldClass:
            __doc__ = DocDescr()
        class NewClass(object):
            __doc__ = DocDescr()
        self.assertEqual(OldClass.__doc__, 'object=None; type=OldClass')
        self.assertEqual(OldClass().__doc__, 'object=OldClass instance; type=OldClass')
        self.assertEqual(NewClass.__doc__, 'object=None; type=NewClass')
        self.assertEqual(NewClass().__doc__, 'object=NewClass instance; type=NewClass')

    def test_set_class(self):
        # Testing __class__ assignment...
        class C(object): pass
        class D(object): pass
        class E(object): pass
        class F(D, E): pass
        for cls in C, D, E, F:
            for cls2 in C, D, E, F:
                x = cls()
                x.__class__ = cls2
                self.assertIs(x.__class__, cls2)
                x.__class__ = cls
                self.assertIs(x.__class__, cls)
        def cant(x, C):
            try:
                x.__class__ = C
            except TypeError:
                pass
            else:
                self.fail("shouldn't allow %r.__class__ = %r" % (x, C))
            try:
                delattr(x, "__class__")
            except (TypeError, AttributeError):
                pass
            else:
                self.fail("shouldn't allow del %r.__class__" % x)
        cant(C(), list)
        cant(list(), C)
        cant(C(), 1)
        cant(C(), object)
        cant(object(), list)
        cant(list(), object)
        class Int(int): __slots__ = []
        cant(2, Int)
        cant(Int(), int)
        cant(True, int)
        cant(2, bool)
        o = object()
        cant(o, type(1))
        cant(o, type(None))
        del o
        class G(object):
            __slots__ = ["a", "b"]
        class H(object):
            __slots__ = ["b", "a"]
        try:
            unicode
        except NameError:
            class I(object):
                __slots__ = ["a", "b"]
        else:
            class I(object):
                __slots__ = [unicode("a"), unicode("b")]
        class J(object):
            __slots__ = ["c", "b"]
        class K(object):
            __slots__ = ["a", "b", "d"]
        class L(H):
            __slots__ = ["e"]
        class M(I):
            __slots__ = ["e"]
        class N(J):
            __slots__ = ["__weakref__"]
        class P(J):
            __slots__ = ["__dict__"]
        class Q(J):
            pass
        class R(J):
            __slots__ = ["__dict__", "__weakref__"]

        for cls, cls2 in ((G, H), (G, I), (I, H), (Q, R), (R, Q)):
            x = cls()
            x.a = 1
            x.__class__ = cls2
            self.assertIs(x.__class__, cls2,
                   "assigning %r as __class__ for %r silently failed" % (cls2, x))
            self.assertEqual(x.a, 1)
            x.__class__ = cls
            self.assertIs(x.__class__, cls,
                   "assigning %r as __class__ for %r silently failed" % (cls, x))
            self.assertEqual(x.a, 1)
        for cls in G, J, K, L, M, N, P, R, list, Int:
            for cls2 in G, J, K, L, M, N, P, R, list, Int:
                if cls is cls2:
                    continue
                cant(cls(), cls2)

        # Issue5283: when __class__ changes in __del__, the wrong
        # type gets DECREF'd.
        class O(object):
            pass
        class A(object):
            def __del__(self):
                self.__class__ = O
        l = [A() for x in range(100)]
        del l

    def test_set_dict(self):
        # Testing __dict__ assignment...
        class C(object): pass
        a = C()
        a.__dict__ = {'b': 1}
        self.assertEqual(a.b, 1)
        def cant(x, dict):
            try:
                x.__dict__ = dict
            except (AttributeError, TypeError):
                pass
            else:
                self.fail("shouldn't allow %r.__dict__ = %r" % (x, dict))
        cant(a, None)
        cant(a, [])
        cant(a, 1)
        del a.__dict__ # Deleting __dict__ is allowed

        class Base(object):
            pass
        def verify_dict_readonly(x):
            """
            x has to be an instance of a class inheriting from Base.
            """
            cant(x, {})
            try:
                del x.__dict__
            except (AttributeError, TypeError):
                pass
            else:
                self.fail("shouldn't allow del %r.__dict__" % x)
            dict_descr = Base.__dict__["__dict__"]
            try:
                dict_descr.__set__(x, {})
            except (AttributeError, TypeError):
                pass
            else:
                self.fail("dict_descr allowed access to %r's dict" % x)

        # Classes don't allow __dict__ assignment and have readonly dicts
        class Meta1(type, Base):
            pass
        class Meta2(Base, type):
            pass
        class D(object):
            __metaclass__ = Meta1
        class E(object):
            __metaclass__ = Meta2
        for cls in C, D, E:
            verify_dict_readonly(cls)
            class_dict = cls.__dict__
            try:
                class_dict["spam"] = "eggs"
            except TypeError:
                pass
            else:
                self.fail("%r's __dict__ can be modified" % cls)

        # Modules also disallow __dict__ assignment
        class Module1(types.ModuleType, Base):
            pass
        class Module2(Base, types.ModuleType):
            pass
        for ModuleType in Module1, Module2:
            mod = ModuleType("spam")
            verify_dict_readonly(mod)
            mod.__dict__["spam"] = "eggs"

        # Exception's __dict__ can be replaced, but not deleted
        # (at least not any more than regular exception's __dict__ can
        # be deleted; on CPython it is not the case, whereas on PyPy they
        # can, just like any other new-style instance's __dict__.)
        def can_delete_dict(e):
            try:
                del e.__dict__
            except (TypeError, AttributeError):
                return False
            else:
                return True
        class Exception1(Exception, Base):
            pass
        class Exception2(Base, Exception):
            pass
        for ExceptionType in Exception, Exception1, Exception2:
            e = ExceptionType()
            e.__dict__ = {"a": 1}
            self.assertEqual(e.a, 1)
            self.assertEqual(can_delete_dict(e), can_delete_dict(ValueError()))

    def test_pickles(self):
        # Testing pickling and copying new-style classes and objects...
        import pickle, cPickle

        def sorteditems(d):
            L = d.items()
            L.sort()
            return L

        global C
        class C(object):
            def __init__(self, a, b):
                super(C, self).__init__()
                self.a = a
                self.b = b
            def __repr__(self):
                return "C(%r, %r)" % (self.a, self.b)

        global C1
        class C1(list):
            def __new__(cls, a, b):
                return super(C1, cls).__new__(cls)
            def __getnewargs__(self):
                return (self.a, self.b)
            def __init__(self, a, b):
                self.a = a
                self.b = b
            def __repr__(self):
                return "C1(%r, %r)<%r>" % (self.a, self.b, list(self))

        global C2
        class C2(int):
            def __new__(cls, a, b, val=0):
                return super(C2, cls).__new__(cls, val)
            def __getnewargs__(self):
                return (self.a, self.b, int(self))
            def __init__(self, a, b, val=0):
                self.a = a
                self.b = b
            def __repr__(self):
                return "C2(%r, %r)<%r>" % (self.a, self.b, int(self))

        global C3
        class C3(object):
            def __init__(self, foo):
                self.foo = foo
            def __getstate__(self):
                return self.foo
            def __setstate__(self, foo):
                self.foo = foo

        global C4classic, C4
        class C4classic: # classic
            pass
        class C4(C4classic, object): # mixed inheritance
            pass

        for p in pickle, cPickle:
            for bin in range(p.HIGHEST_PROTOCOL + 1):
                for cls in C, C1, C2:
                    s = p.dumps(cls, bin)
                    cls2 = p.loads(s)
                    self.assertIs(cls2, cls)

                a = C1(1, 2); a.append(42); a.append(24)
                b = C2("hello", "world", 42)
                s = p.dumps((a, b), bin)
                x, y = p.loads(s)
                self.assertEqual(x.__class__, a.__class__)
                self.assertEqual(sorteditems(x.__dict__), sorteditems(a.__dict__))
                self.assertEqual(y.__class__, b.__class__)
                self.assertEqual(sorteditems(y.__dict__), sorteditems(b.__dict__))
                self.assertEqual(repr(x), repr(a))
                self.assertEqual(repr(y), repr(b))
                # Test for __getstate__ and __setstate__ on new style class
                u = C3(42)
                s = p.dumps(u, bin)
                v = p.loads(s)
                self.assertEqual(u.__class__, v.__class__)
                self.assertEqual(u.foo, v.foo)
                # Test for picklability of hybrid class
                u = C4()
                u.foo = 42
                s = p.dumps(u, bin)
                v = p.loads(s)
                self.assertEqual(u.__class__, v.__class__)
                self.assertEqual(u.foo, v.foo)

        # Testing copy.deepcopy()
        import copy
        for cls in C, C1, C2:
            cls2 = copy.deepcopy(cls)
            self.assertIs(cls2, cls)

        a = C1(1, 2); a.append(42); a.append(24)
        b = C2("hello", "world", 42)
        x, y = copy.deepcopy((a, b))
        self.assertEqual(x.__class__, a.__class__)
        self.assertEqual(sorteditems(x.__dict__), sorteditems(a.__dict__))
        self.assertEqual(y.__class__, b.__class__)
        self.assertEqual(sorteditems(y.__dict__), sorteditems(b.__dict__))
        self.assertEqual(repr(x), repr(a))
        self.assertEqual(repr(y), repr(b))

    def test_pickle_slots(self):
        # Testing pickling of classes with __slots__ ...
        import pickle, cPickle
        # Pickling of classes with __slots__ but without __getstate__ should fail
        global B, C, D, E
        class B(object):
            pass
        for base in [object, B]:
            class C(base):
                __slots__ = ['a']
            class D(C):
                pass
            for proto in range(2):
                try:
                    pickle.dumps(C(), proto)
                except TypeError:
                    pass
                else:
                    self.fail("should fail: pickle C instance - %s" % base)
                try:
                    cPickle.dumps(C(), proto)
                except TypeError:
                    pass
                else:
                    self.fail("should fail: cPickle C instance - %s" % base)
                try:
                    pickle.dumps(C(), proto)
                except TypeError:
                    pass
                else:
                    self.fail("should fail: pickle D instance - %s" % base)
                try:
                    cPickle.dumps(D(), proto)
                except TypeError:
                    pass
                else:
                    self.fail("should fail: cPickle D instance - %s" % base)
            # Give C a nice generic __getstate__ and __setstate__
            class C(base):
                __slots__ = ['a']
                def __getstate__(self):
                    try:
                        d = self.__dict__.copy()
                    except AttributeError:
                        d = {}
                    for cls in self.__class__.__mro__:
                        for sn in cls.__dict__.get('__slots__', ()):
                            try:
                                d[sn] = getattr(self, sn)
                            except AttributeError:
                                pass
                    return d
                def __setstate__(self, d):
                    for k, v in d.items():
                        setattr(self, k, v)
            class D(C):
                pass
            # Now it should work
            x = C()
            for proto in range(pickle.HIGHEST_PROTOCOL + 1):
                y = pickle.loads(pickle.dumps(x, proto))
                self.assertNotHasAttr(y, 'a')
                y = cPickle.loads(cPickle.dumps(x, proto))
                self.assertNotHasAttr(y, 'a')
            x.a = 42
            for proto in range(pickle.HIGHEST_PROTOCOL + 1):
                y = pickle.loads(pickle.dumps(x, proto))
                self.assertEqual(y.a, 42)
                y = cPickle.loads(cPickle.dumps(x, proto))
                self.assertEqual(y.a, 42)
            x = D()
            x.a = 42
            x.b = 100
            for proto in range(pickle.HIGHEST_PROTOCOL + 1):
                y = pickle.loads(pickle.dumps(x, proto))
                self.assertEqual(y.a + y.b, 142)
                y = cPickle.loads(cPickle.dumps(x, proto))
                self.assertEqual(y.a + y.b, 142)
            # A subclass that adds a slot should also work
            class E(C):
                __slots__ = ['b']
            x = E()
            x.a = 42
            x.b = "foo"
            for proto in range(pickle.HIGHEST_PROTOCOL + 1):
                y = pickle.loads(pickle.dumps(x, proto))
                self.assertEqual(y.a, x.a)
                self.assertEqual(y.b, x.b)
                y = cPickle.loads(cPickle.dumps(x, proto))
                self.assertEqual(y.a, x.a)
                self.assertEqual(y.b, x.b)

    def test_binary_operator_override(self):
        # Testing overrides of binary operations...
        class I(int):
            def __repr__(self):
                return "I(%r)" % int(self)
            def __add__(self, other):
                return I(int(self) + int(other))
            __radd__ = __add__
            def __pow__(self, other, mod=None):
                if mod is None:
                    return I(pow(int(self), int(other)))
                else:
                    return I(pow(int(self), int(other), int(mod)))
            def __rpow__(self, other, mod=None):
                if mod is None:
                    return I(pow(int(other), int(self), mod))
                else:
                    return I(pow(int(other), int(self), int(mod)))

        self.assertEqual(repr(I(1) + I(2)), "I(3)")
        self.assertEqual(repr(I(1) + 2), "I(3)")
        self.assertEqual(repr(1 + I(2)), "I(3)")
        self.assertEqual(repr(I(2) ** I(3)), "I(8)")
        self.assertEqual(repr(2 ** I(3)), "I(8)")
        self.assertEqual(repr(I(2) ** 3), "I(8)")
        self.assertEqual(repr(pow(I(2), I(3), I(5))), "I(3)")
        class S(str):
            def __eq__(self, other):
                return self.lower() == other.lower()
            __hash__ = None # Silence Py3k warning

    def test_subclass_propagation(self):
        # Testing propagation of slot functions to subclasses...
        class A(object):
            pass
        class B(A):
            pass
        class C(A):
            pass
        class D(B, C):
            pass
        d = D()
        orig_hash = hash(d) # related to id(d) in platform-dependent ways
        A.__hash__ = lambda self: 42
        self.assertEqual(hash(d), 42)
        C.__hash__ = lambda self: 314
        self.assertEqual(hash(d), 314)
        B.__hash__ = lambda self: 144
        self.assertEqual(hash(d), 144)
        D.__hash__ = lambda self: 100
        self.assertEqual(hash(d), 100)
        D.__hash__ = None
        self.assertRaises(TypeError, hash, d)
        del D.__hash__
        self.assertEqual(hash(d), 144)
        B.__hash__ = None
        self.assertRaises(TypeError, hash, d)
        del B.__hash__
        self.assertEqual(hash(d), 314)
        C.__hash__ = None
        self.assertRaises(TypeError, hash, d)
        del C.__hash__
        self.assertEqual(hash(d), 42)
        A.__hash__ = None
        self.assertRaises(TypeError, hash, d)
        del A.__hash__
        self.assertEqual(hash(d), orig_hash)
        d.foo = 42
        d.bar = 42
        self.assertEqual(d.foo, 42)
        self.assertEqual(d.bar, 42)
        def __getattribute__(self, name):
            if name == "foo":
                return 24
            return object.__getattribute__(self, name)
        A.__getattribute__ = __getattribute__
        self.assertEqual(d.foo, 24)
        self.assertEqual(d.bar, 42)
        def __getattr__(self, name):
            if name in ("spam", "foo", "bar"):
                return "hello"
            raise AttributeError, name
        B.__getattr__ = __getattr__
        self.assertEqual(d.spam, "hello")
        self.assertEqual(d.foo, 24)
        self.assertEqual(d.bar, 42)
        del A.__getattribute__
        self.assertEqual(d.foo, 42)
        del d.foo
        self.assertEqual(d.foo, "hello")
        self.assertEqual(d.bar, 42)
        del B.__getattr__
        try:
            d.foo
        except AttributeError:
            pass
        else:
            self.fail("d.foo should be undefined now")

        # Test a nasty bug in recurse_down_subclasses()
        class A(object):
            pass
        class B(A):
            pass
        del B
        test_support.gc_collect()
        A.__setitem__ = lambda *a: None # crash

    def test_buffer_inheritance(self):
        # Testing that buffer interface is inherited ...

        import binascii
        # SF bug [#470040] ParseTuple t# vs subclasses.

        class MyStr(str):
            pass
        base = 'abc'
        m = MyStr(base)
        # b2a_hex uses the buffer interface to get its argument's value, via
        # PyArg_ParseTuple 't#' code.
        self.assertEqual(binascii.b2a_hex(m), binascii.b2a_hex(base))

        # It's not clear that unicode will continue to support the character
        # buffer interface, and this test will fail if that's taken away.
        class MyUni(unicode):
            pass
        base = u'abc'
        m = MyUni(base)
        self.assertEqual(binascii.b2a_hex(m), binascii.b2a_hex(base))

        class MyInt(int):
            pass
        m = MyInt(42)
        try:
            binascii.b2a_hex(m)
            self.fail('subclass of int should not have a buffer interface')
        except TypeError:
            pass

    def test_str_of_str_subclass(self):
        # Testing __str__ defined in subclass of str ...
        import binascii
        import cStringIO

        class octetstring(str):
            def __str__(self):
                return binascii.b2a_hex(self)
            def __repr__(self):
                return self + " repr"

        o = octetstring('A')
        self.assertEqual(type(o), octetstring)
        self.assertEqual(type(str(o)), str)
        self.assertEqual(type(repr(o)), str)
        self.assertEqual(ord(o), 0x41)
        self.assertEqual(str(o), '41')
        self.assertEqual(repr(o), 'A repr')
        self.assertEqual(o.__str__(), '41')
        self.assertEqual(o.__repr__(), 'A repr')

        capture = cStringIO.StringIO()
        # Calling str() or not exercises different internal paths.
        print >> capture, o
        print >> capture, str(o)
        self.assertEqual(capture.getvalue(), '41\n41\n')
        capture.close()

    def test_keyword_arguments(self):
        # Testing keyword arguments to __init__, __call__...
        def f(a): return a
        self.assertEqual(f.__call__(a=42), 42)
        a = []
        list.__init__(a, sequence=[0, 1, 2])
        self.assertEqual(a, [0, 1, 2])

    def test_recursive_call(self):
        # Testing recursive __call__() by setting to instance of class...
        class A(object):
            pass

        A.__call__ = A()
        try:
            A()()
        except RuntimeError:
            pass
        else:
            self.fail("Recursion limit should have been reached for __call__()")

    def test_delete_hook(self):
        # Testing __del__ hook...
        log = []
        class C(object):
            def __del__(self):
                log.append(1)
        c = C()
        self.assertEqual(log, [])
        del c
        test_support.gc_collect()
        self.assertEqual(log, [1])

        class D(object): pass
        d = D()
        try: del d[0]
        except TypeError: pass
        else: self.fail("invalid del() didn't raise TypeError")

    def test_hash_inheritance(self):
        # Testing hash of mutable subclasses...

        class mydict(dict):
            pass
        d = mydict()
        try:
            hash(d)
        except TypeError:
            pass
        else:
            self.fail("hash() of dict subclass should fail")

        class mylist(list):
            pass
        d = mylist()
        try:
            hash(d)
        except TypeError:
            pass
        else:
            self.fail("hash() of list subclass should fail")

    def test_str_operations(self):
        try: 'a' + 5
        except TypeError: pass
        else: self.fail("'' + 5 doesn't raise TypeError")

        try: ''.split('')
        except ValueError: pass
        else: self.fail("''.split('') doesn't raise ValueError")

        try: ''.join([0])
        except TypeError: pass
        else: self.fail("''.join([0]) doesn't raise TypeError")

        try: ''.rindex('5')
        except ValueError: pass
        else: self.fail("''.rindex('5') doesn't raise ValueError")

        try: '%(n)s' % None
        except TypeError: pass
        else: self.fail("'%(n)s' % None doesn't raise TypeError")

        try: '%(n' % {}
        except ValueError: pass
        else: self.fail("'%(n' % {} '' doesn't raise ValueError")

        try: '%*s' % ('abc')
        except TypeError: pass
        else: self.fail("'%*s' % ('abc') doesn't raise TypeError")

        try: '%*.*s' % ('abc', 5)
        except TypeError: pass
        else: self.fail("'%*.*s' % ('abc', 5) doesn't raise TypeError")

        try: '%s' % (1, 2)
        except TypeError: pass
        else: self.fail("'%s' % (1, 2) doesn't raise TypeError")

        try: '%' % None
        except ValueError: pass
        else: self.fail("'%' % None doesn't raise ValueError")

        self.assertEqual('534253'.isdigit(), 1)
        self.assertEqual('534253x'.isdigit(), 0)
        self.assertEqual('%c' % 5, '\x05')
        self.assertEqual('%c' % '5', '5')

    def test_deepcopy_recursive(self):
        # Testing deepcopy of recursive objects...
        class Node:
            pass
        a = Node()
        b = Node()
        a.b = b
        b.a = a
        z = deepcopy(a) # This blew up before

    def test_unintialized_modules(self):
        # Testing uninitialized module objects...
        from types import ModuleType as M
        m = M.__new__(M)
        str(m)
        self.assertNotHasAttr(m, "__name__")
        self.assertNotHasAttr(m, "__file__")
        self.assertNotHasAttr(m, "foo")
        self.assertFalse(m.__dict__)   # None or {} are both reasonable answers
        m.foo = 1
        self.assertEqual(m.__dict__, {"foo": 1})

    def test_funny_new(self):
        # Testing __new__ returning something unexpected...
        class C(object):
            def __new__(cls, arg):
                if isinstance(arg, str): return [1, 2, 3]
                elif isinstance(arg, int): return object.__new__(D)
                else: return object.__new__(cls)
        class D(C):
            def __init__(self, arg):
                self.foo = arg
        self.assertEqual(C("1"), [1, 2, 3])
        self.assertEqual(D("1"), [1, 2, 3])
        d = D(None)
        self.assertEqual(d.foo, None)
        d = C(1)
        self.assertEqual(isinstance(d, D), True)
        self.assertEqual(d.foo, 1)
        d = D(1)
        self.assertEqual(isinstance(d, D), True)
        self.assertEqual(d.foo, 1)

    def test_imul_bug(self):
        # Testing for __imul__ problems...
        # SF bug 544647
        class C(object):
            def __imul__(self, other):
                return (self, other)
        x = C()
        y = x
        y *= 1.0
        self.assertEqual(y, (x, 1.0))
        y = x
        y *= 2
        self.assertEqual(y, (x, 2))
        y = x
        y *= 3L
        self.assertEqual(y, (x, 3L))
        y = x
        y *= 1L<<100
        self.assertEqual(y, (x, 1L<<100))
        y = x
        y *= None
        self.assertEqual(y, (x, None))
        y = x
        y *= "foo"
        self.assertEqual(y, (x, "foo"))

    def test_copy_setstate(self):
        # Testing that copy.*copy() correctly uses __setstate__...
        import copy
        class C(object):
            def __init__(self, foo=None):
                self.foo = foo
                self.__foo = foo
            def setfoo(self, foo=None):
                self.foo = foo
            def getfoo(self):
                return self.__foo
            def __getstate__(self):
                return [self.foo]
            def __setstate__(self_, lst):
                self.assertEqual(len(lst), 1)
                self_.__foo = self_.foo = lst[0]
        a = C(42)
        a.setfoo(24)
        self.assertEqual(a.foo, 24)
        self.assertEqual(a.getfoo(), 42)
        b = copy.copy(a)
        self.assertEqual(b.foo, 24)
        self.assertEqual(b.getfoo(), 24)
        b = copy.deepcopy(a)
        self.assertEqual(b.foo, 24)
        self.assertEqual(b.getfoo(), 24)

    def test_slices(self):
        # Testing cases with slices and overridden __getitem__ ...

        # Strings
        self.assertEqual("hello"[:4], "hell")
        self.assertEqual("hello"[slice(4)], "hell")
        self.assertEqual(str.__getitem__("hello", slice(4)), "hell")
        class S(str):
            def __getitem__(self, x):
                return str.__getitem__(self, x)
        self.assertEqual(S("hello")[:4], "hell")
        self.assertEqual(S("hello")[slice(4)], "hell")
        self.assertEqual(S("hello").__getitem__(slice(4)), "hell")
        # Tuples
        self.assertEqual((1,2,3)[:2], (1,2))
        self.assertEqual((1,2,3)[slice(2)], (1,2))
        self.assertEqual(tuple.__getitem__((1,2,3), slice(2)), (1,2))
        class T(tuple):
            def __getitem__(self, x):
                return tuple.__getitem__(self, x)
        self.assertEqual(T((1,2,3))[:2], (1,2))
        self.assertEqual(T((1,2,3))[slice(2)], (1,2))
        self.assertEqual(T((1,2,3)).__getitem__(slice(2)), (1,2))
        # Lists
        self.assertEqual([1,2,3][:2], [1,2])
        self.assertEqual([1,2,3][slice(2)], [1,2])
        self.assertEqual(list.__getitem__([1,2,3], slice(2)), [1,2])
        class L(list):
            def __getitem__(self, x):
                return list.__getitem__(self, x)
        self.assertEqual(L([1,2,3])[:2], [1,2])
        self.assertEqual(L([1,2,3])[slice(2)], [1,2])
        self.assertEqual(L([1,2,3]).__getitem__(slice(2)), [1,2])
        # Now do lists and __setitem__
        a = L([1,2,3])
        a[slice(1, 3)] = [3,2]
        self.assertEqual(a, [1,3,2])
        a[slice(0, 2, 1)] = [3,1]
        self.assertEqual(a, [3,1,2])
        a.__setitem__(slice(1, 3), [2,1])
        self.assertEqual(a, [3,2,1])
        a.__setitem__(slice(0, 2, 1), [2,3])
        self.assertEqual(a, [2,3,1])

    def test_subtype_resurrection(self):
        # Testing resurrection of new-style instance...

        class C(object):
            container = []

            def __del__(self):
                # resurrect the instance
                C.container.append(self)

        c = C()
        c.attr = 42

        # The most interesting thing here is whether this blows up, due to
        # flawed GC tracking logic in typeobject.c's call_finalizer() (a 2.2.1
        # bug).
        del c

        # If that didn't blow up, it's also interesting to see whether clearing
        # the last container slot works: that will attempt to delete c again,
        # which will cause c to get appended back to the container again
        # "during" the del.  (On non-CPython implementations, however, __del__
        # is typically not called again.)
        test_support.gc_collect()
        self.assertEqual(len(C.container), 1)
        del C.container[-1]
        if test_support.check_impl_detail():
            test_support.gc_collect()
            self.assertEqual(len(C.container), 1)
            self.assertEqual(C.container[-1].attr, 42)

        # Make c mortal again, so that the test framework with -l doesn't report
        # it as a leak.
        del C.__del__

    def test_slots_trash(self):
        # Testing slot trash...
        # Deallocating deeply nested slotted trash caused stack overflows
        class trash(object):
            __slots__ = ['x']
            def __init__(self, x):
                self.x = x
        o = None
        for i in xrange(50000):
            o = trash(o)
        del o

    def test_slots_multiple_inheritance(self):
        # SF bug 575229, multiple inheritance w/ slots dumps core
        class A(object):
            __slots__=()
        class B(object):
            pass
        class C(A,B) :
            __slots__=()
        if test_support.check_impl_detail():
            self.assertEqual(C.__basicsize__, B.__basicsize__)
        self.assertHasAttr(C, '__dict__')
        self.assertHasAttr(C, '__weakref__')
        C().x = 2

    def test_rmul(self):
        # Testing correct invocation of __rmul__...
        # SF patch 592646
        class C(object):
            def __mul__(self, other):
                return "mul"
            def __rmul__(self, other):
                return "rmul"
        a = C()
        self.assertEqual(a*2, "mul")
        self.assertEqual(a*2.2, "mul")
        self.assertEqual(2*a, "rmul")
        self.assertEqual(2.2*a, "rmul")

    def test_ipow(self):
        # Testing correct invocation of __ipow__...
        # [SF bug 620179]
        class C(object):
            def __ipow__(self, other):
                pass
        a = C()
        a **= 2

    def test_mutable_bases(self):
        # Testing mutable bases...

        # stuff that should work:
        class C(object):
            pass
        class C2(object):
            def __getattribute__(self, attr):
                if attr == 'a':
                    return 2
                else:
                    return super(C2, self).__getattribute__(attr)
            def meth(self):
                return 1
        class D(C):
            pass
        class E(D):
            pass
        d = D()
        e = E()
        D.__bases__ = (C,)
        D.__bases__ = (C2,)
        self.assertEqual(d.meth(), 1)
        self.assertEqual(e.meth(), 1)
        self.assertEqual(d.a, 2)
        self.assertEqual(e.a, 2)
        self.assertEqual(C2.__subclasses__(), [D])

        try:
            del D.__bases__
        except (TypeError, AttributeError):
            pass
        else:
            self.fail("shouldn't be able to delete .__bases__")

        try:
            D.__bases__ = ()
        except TypeError, msg:
            if str(msg) == "a new-style class can't have only classic bases":
                self.fail("wrong error message for .__bases__ = ()")
        else:
            self.fail("shouldn't be able to set .__bases__ to ()")

        try:
            D.__bases__ = (D,)
        except TypeError:
            pass
        else:
            # actually, we'll have crashed by here...
            self.fail("shouldn't be able to create inheritance cycles")

        try:
            D.__bases__ = (C, C)
        except TypeError:
            pass
        else:
            self.fail("didn't detect repeated base classes")

        try:
            D.__bases__ = (E,)
        except TypeError:
            pass
        else:
            self.fail("shouldn't be able to create inheritance cycles")

        # let's throw a classic class into the mix:
        class Classic:
            def meth2(self):
                return 3

        D.__bases__ = (C, Classic)

        self.assertEqual(d.meth2(), 3)
        self.assertEqual(e.meth2(), 3)
        try:
            d.a
        except AttributeError:
            pass
        else:
            self.fail("attribute should have vanished")

        try:
            D.__bases__ = (Classic,)
        except TypeError:
            pass
        else:
            self.fail("new-style class must have a new-style base")

    def test_builtin_bases(self):
        # Make sure all the builtin types can have their base queried without
        # segfaulting. See issue #5787.
        builtin_types = [tp for tp in __builtin__.__dict__.itervalues()
                         if isinstance(tp, type)]
        for tp in builtin_types:
            object.__getattribute__(tp, "__bases__")
            if tp is not object:
                self.assertEqual(len(tp.__bases__), 1, tp)

        class L(list):
            pass

        class C(object):
            pass

        class D(C):
            pass

        try:
            L.__bases__ = (dict,)
        except TypeError:
            pass
        else:
            self.fail("shouldn't turn list subclass into dict subclass")

        try:
            list.__bases__ = (dict,)
        except TypeError:
            pass
        else:
            self.fail("shouldn't be able to assign to list.__bases__")

        try:
            D.__bases__ = (C, list)
        except TypeError:
            pass
        else:
            assert 0, "best_base calculation found wanting"


    def test_mutable_bases_with_failing_mro(self):
        # Testing mutable bases with failing mro...
        class WorkOnce(type):
            def __new__(self, name, bases, ns):
                self.flag = 0
                return super(WorkOnce, self).__new__(WorkOnce, name, bases, ns)
            def mro(self):
                if self.flag > 0:
                    raise RuntimeError, "bozo"
                else:
                    self.flag += 1
                    return type.mro(self)

        class WorkAlways(type):
            def mro(self):
                # this is here to make sure that .mro()s aren't called
                # with an exception set (which was possible at one point).
                # An error message will be printed in a debug build.
                # What's a good way to test for this?
                return type.mro(self)

        class C(object):
            pass

        class C2(object):
            pass

        class D(C):
            pass

        class E(D):
            pass

        class F(D):
            __metaclass__ = WorkOnce

        class G(D):
            __metaclass__ = WorkAlways

        # Immediate subclasses have their mro's adjusted in alphabetical
        # order, so E's will get adjusted before adjusting F's fails.  We
        # check here that E's gets restored.

        E_mro_before = E.__mro__
        D_mro_before = D.__mro__

        try:
            D.__bases__ = (C2,)
        except RuntimeError:
            self.assertEqual(E.__mro__, E_mro_before)
            self.assertEqual(D.__mro__, D_mro_before)
        else:
            self.fail("exception not propagated")

    def test_mutable_bases_catch_mro_conflict(self):
        # Testing mutable bases catch mro conflict...
        class A(object):
            pass

        class B(object):
            pass

        class C(A, B):
            pass

        class D(A, B):
            pass

        class E(C, D):
            pass

        try:
            C.__bases__ = (B, A)
        except TypeError:
            pass
        else:
            self.fail("didn't catch MRO conflict")

    def test_mutable_names(self):
        # Testing mutable names...
        class C(object):
            pass

        # C.__module__ could be 'test_descr' or '__main__'
        mod = C.__module__

        C.__name__ = 'D'
        self.assertEqual((C.__module__, C.__name__), (mod, 'D'))

        C.__name__ = 'D.E'
        self.assertEqual((C.__module__, C.__name__), (mod, 'D.E'))

    def test_evil_type_name(self):
        # A badly placed Py_DECREF in type_set_name led to arbitrary code
        # execution while the type structure was not in a sane state, and a
        # possible segmentation fault as a result.  See bug #16447.
        class Nasty(str):
            def __del__(self):
                C.__name__ = "other"

        class C(object):
            pass

        C.__name__ = Nasty("abc")
        C.__name__ = "normal"

    def test_subclass_right_op(self):
        # Testing correct dispatch of subclass overloading __r<op>__...

        # This code tests various cases where right-dispatch of a subclass
        # should be preferred over left-dispatch of a base class.

        # Case 1: subclass of int; this tests code in abstract.c::binary_op1()

        class B(int):
            def __floordiv__(self, other):
                return "B.__floordiv__"
            def __rfloordiv__(self, other):
                return "B.__rfloordiv__"

        self.assertEqual(B(1) // 1, "B.__floordiv__")
        self.assertEqual(1 // B(1), "B.__rfloordiv__")

        # Case 2: subclass of object; this is just the baseline for case 3

        class C(object):
            def __floordiv__(self, other):
                return "C.__floordiv__"
            def __rfloordiv__(self, other):
                return "C.__rfloordiv__"

        self.assertEqual(C() // 1, "C.__floordiv__")
        self.assertEqual(1 // C(), "C.__rfloordiv__")

        # Case 3: subclass of new-style class; here it gets interesting

        class D(C):
            def __floordiv__(self, other):
                return "D.__floordiv__"
            def __rfloordiv__(self, other):
                return "D.__rfloordiv__"

        self.assertEqual(D() // C(), "D.__floordiv__")
        self.assertEqual(C() // D(), "D.__rfloordiv__")

        # Case 4: this didn't work right in 2.2.2 and 2.3a1

        class E(C):
            pass

        self.assertEqual(E.__rfloordiv__, C.__rfloordiv__)

        self.assertEqual(E() // 1, "C.__floordiv__")
        self.assertEqual(1 // E(), "C.__rfloordiv__")
        self.assertEqual(E() // C(), "C.__floordiv__")
        self.assertEqual(C() // E(), "C.__floordiv__") # This one would fail

    @test_support.impl_detail("testing an internal kind of method object")
    def test_meth_class_get(self):
        # Testing __get__ method of METH_CLASS C methods...
        # Full coverage of descrobject.c::classmethod_get()

        # Baseline
        arg = [1, 2, 3]
        res = {1: None, 2: None, 3: None}
        self.assertEqual(dict.fromkeys(arg), res)
        self.assertEqual({}.fromkeys(arg), res)

        # Now get the descriptor
        descr = dict.__dict__["fromkeys"]

        # More baseline using the descriptor directly
        self.assertEqual(descr.__get__(None, dict)(arg), res)
        self.assertEqual(descr.__get__({})(arg), res)

        # Now check various error cases
        try:
            descr.__get__(None, None)
        except TypeError:
            pass
        else:
            self.fail("shouldn't have allowed descr.__get__(None, None)")
        try:
            descr.__get__(42)
        except TypeError:
            pass
        else:
            self.fail("shouldn't have allowed descr.__get__(42)")
        try:
            descr.__get__(None, 42)
        except TypeError:
            pass
        else:
            self.fail("shouldn't have allowed descr.__get__(None, 42)")
        try:
            descr.__get__(None, int)
        except TypeError:
            pass
        else:
            self.fail("shouldn't have allowed descr.__get__(None, int)")

    def test_isinst_isclass(self):
        # Testing proxy isinstance() and isclass()...
        class Proxy(object):
            def __init__(self, obj):
                self.__obj = obj
            def __getattribute__(self, name):
                if name.startswith("_Proxy__"):
                    return object.__getattribute__(self, name)
                else:
                    return getattr(self.__obj, name)
        # Test with a classic class
        class C:
            pass
        a = C()
        pa = Proxy(a)
        self.assertIsInstance(a, C)  # Baseline
        self.assertIsInstance(pa, C) # Test
        # Test with a classic subclass
        class D(C):
            pass
        a = D()
        pa = Proxy(a)
        self.assertIsInstance(a, C)  # Baseline
        self.assertIsInstance(pa, C) # Test
        # Test with a new-style class
        class C(object):
            pass
        a = C()
        pa = Proxy(a)
        self.assertIsInstance(a, C)  # Baseline
        self.assertIsInstance(pa, C) # Test
        # Test with a new-style subclass
        class D(C):
            pass
        a = D()
        pa = Proxy(a)
        self.assertIsInstance(a, C)  # Baseline
        self.assertIsInstance(pa, C) # Test

    def test_proxy_super(self):
        # Testing super() for a proxy object...
        class Proxy(object):
            def __init__(self, obj):
                self.__obj = obj
            def __getattribute__(self, name):
                if name.startswith("_Proxy__"):
                    return object.__getattribute__(self, name)
                else:
                    return getattr(self.__obj, name)

        class B(object):
            def f(self):
                return "B.f"

        class C(B):
            def f(self):
                return super(C, self).f() + "->C.f"

        obj = C()
        p = Proxy(obj)
        self.assertEqual(C.__dict__["f"](p), "B.f->C.f")

    def test_carloverre(self):
        # Testing prohibition of Carlo Verre's hack...
        try:
            object.__setattr__(str, "foo", 42)
        except TypeError:
            pass
        else:
            self.fail("Carlo Verre __setattr__ succeeded!")
        try:
            object.__delattr__(str, "lower")
        except TypeError:
            pass
        else:
            self.fail("Carlo Verre __delattr__ succeeded!")

    def test_weakref_segfault(self):
        # Testing weakref segfault...
        # SF 742911
        import weakref

        class Provoker:
            def __init__(self, referrent):
                self.ref = weakref.ref(referrent)

            def __del__(self):
                x = self.ref()

        class Oops(object):
            pass

        o = Oops()
        o.whatever = Provoker(o)
        del o

    def test_wrapper_segfault(self):
        # SF 927248: deeply nested wrappers could cause stack overflow
        f = lambda:None
        for i in xrange(1000000):
            f = f.__call__
        f = None

    def test_file_fault(self):
        # Testing sys.stdout is changed in getattr...
        test_stdout = sys.stdout
        class StdoutGuard:
            def __getattr__(self, attr):
                sys.stdout = sys.__stdout__
                raise RuntimeError("Premature access to sys.stdout.%s" % attr)
        sys.stdout = StdoutGuard()
        try:
            print "Oops!"
        except RuntimeError:
            pass
        finally:
            sys.stdout = test_stdout

    def test_vicious_descriptor_nonsense(self):
        # Testing vicious_descriptor_nonsense...

        # A potential segfault spotted by Thomas Wouters in mail to
        # python-dev 2003-04-17, turned into an example & fixed by Michael
        # Hudson just less than four months later...

        class Evil(object):
            def __hash__(self):
                return hash('attr')
            def __eq__(self, other):
                del C.attr
                return 0

        class Descr(object):
            def __get__(self, ob, type=None):
                return 1

        class C(object):
            attr = Descr()

        c = C()
        c.__dict__[Evil()] = 0

        self.assertEqual(c.attr, 1)
        # this makes a crash more likely:
        test_support.gc_collect()
        self.assertNotHasAttr(c, 'attr')

    def test_init(self):
        # SF 1155938
        class Foo(object):
            def __init__(self):
                return 10
        try:
            Foo()
        except TypeError:
            pass
        else:
            self.fail("did not test __init__() for None return")

    def test_method_wrapper(self):
        # Testing method-wrapper objects...
        # <type 'method-wrapper'> did not support any reflection before 2.5

        l = []
        self.assertEqual(l.__add__, l.__add__)
        self.assertEqual(l.__add__, [].__add__)
        self.assertNotEqual(l.__add__, [5].__add__)
        self.assertNotEqual(l.__add__, l.__mul__)
        self.assertEqual(l.__add__.__name__, '__add__')
        if hasattr(l.__add__, '__self__'):
            # CPython
            self.assertIs(l.__add__.__self__, l)
            self.assertIs(l.__add__.__objclass__, list)
        else:
            # Python implementations where [].__add__ is a normal bound method
            self.assertIs(l.__add__.im_self, l)
            self.assertIs(l.__add__.im_class, list)
        self.assertEqual(l.__add__.__doc__, list.__add__.__doc__)
        try:
            hash(l.__add__)
        except TypeError:
            pass
        else:
            self.fail("no TypeError from hash([].__add__)")

        t = ()
        t += (7,)
        self.assertEqual(t.__add__, (7,).__add__)
        self.assertEqual(hash(t.__add__), hash((7,).__add__))

    def test_not_implemented(self):
        # Testing NotImplemented...
        # all binary methods should be able to return a NotImplemented
        import operator

        def specialmethod(self, other):
            return NotImplemented

        def check(expr, x, y):
            try:
                exec expr in {'x': x, 'y': y, 'operator': operator}
            except TypeError:
                pass
            else:
                self.fail("no TypeError from %r" % (expr,))

        N1 = sys.maxint + 1L    # might trigger OverflowErrors instead of
                                # TypeErrors
        N2 = sys.maxint         # if sizeof(int) < sizeof(long), might trigger
                                #   ValueErrors instead of TypeErrors
        for metaclass in [type, types.ClassType]:
            for name, expr, iexpr in [
                    ('__add__',      'x + y',                   'x += y'),
                    ('__sub__',      'x - y',                   'x -= y'),
                    ('__mul__',      'x * y',                   'x *= y'),
                    ('__truediv__',  'operator.truediv(x, y)',  None),
                    ('__floordiv__', 'operator.floordiv(x, y)', None),
                    ('__div__',      'x / y',                   'x /= y'),
                    ('__mod__',      'x % y',                   'x %= y'),
                    ('__divmod__',   'divmod(x, y)',            None),
                    ('__pow__',      'x ** y',                  'x **= y'),
                    ('__lshift__',   'x << y',                  'x <<= y'),
                    ('__rshift__',   'x >> y',                  'x >>= y'),
                    ('__and__',      'x & y',                   'x &= y'),
                    ('__or__',       'x | y',                   'x |= y'),
                    ('__xor__',      'x ^ y',                   'x ^= y'),
                    ('__coerce__',   'coerce(x, y)',            None)]:
                if name == '__coerce__':
                    rname = name
                else:
                    rname = '__r' + name[2:]
                A = metaclass('A', (), {name: specialmethod})
                B = metaclass('B', (), {rname: specialmethod})
                a = A()
                b = B()
                check(expr, a, a)
                check(expr, a, b)
                check(expr, b, a)
                check(expr, b, b)
                check(expr, a, N1)
                check(expr, a, N2)
                check(expr, N1, b)
                check(expr, N2, b)
                if iexpr:
                    check(iexpr, a, a)
                    check(iexpr, a, b)
                    check(iexpr, b, a)
                    check(iexpr, b, b)
                    check(iexpr, a, N1)
                    check(iexpr, a, N2)
                    iname = '__i' + name[2:]
                    C = metaclass('C', (), {iname: specialmethod})
                    c = C()
                    check(iexpr, c, a)
                    check(iexpr, c, b)
                    check(iexpr, c, N1)
                    check(iexpr, c, N2)

    def test_assign_slice(self):
        # ceval.c's assign_slice used to check for
        # tp->tp_as_sequence->sq_slice instead of
        # tp->tp_as_sequence->sq_ass_slice

        class C(object):
            def __setslice__(self, start, stop, value):
                self.value = value

        c = C()
        c[1:2] = 3
        self.assertEqual(c.value, 3)

    def test_set_and_no_get(self):
        # See
        # http://mail.python.org/pipermail/python-dev/2010-January/095637.html
        class Descr(object):

            def __init__(self, name):
                self.name = name

            def __set__(self, obj, value):
                obj.__dict__[self.name] = value
        descr = Descr("a")

        class X(object):
            a = descr

        x = X()
        self.assertIs(x.a, descr)
        x.a = 42
        self.assertEqual(x.a, 42)

        # Also check type_getattro for correctness.
        class Meta(type):
            pass
        class X(object):
            __metaclass__ = Meta
        X.a = 42
        Meta.a = Descr("a")
        self.assertEqual(X.a, 42)

    def test_getattr_hooks(self):
        # issue 4230

        class Descriptor(object):
            counter = 0
            def __get__(self, obj, objtype=None):
                def getter(name):
                    self.counter += 1
                    raise AttributeError(name)
                return getter

        descr = Descriptor()
        class A(object):
            __getattribute__ = descr
        class B(object):
            __getattr__ = descr
        class C(object):
            __getattribute__ = descr
            __getattr__ = descr

        self.assertRaises(AttributeError, getattr, A(), "attr")
        self.assertEqual(descr.counter, 1)
        self.assertRaises(AttributeError, getattr, B(), "attr")
        self.assertEqual(descr.counter, 2)
        self.assertRaises(AttributeError, getattr, C(), "attr")
        self.assertEqual(descr.counter, 4)

        class EvilGetattribute(object):
            # This used to segfault
            def __getattr__(self, name):
                raise AttributeError(name)
            def __getattribute__(self, name):
                del EvilGetattribute.__getattr__
                for i in range(5):
                    gc.collect()
                raise AttributeError(name)

        self.assertRaises(AttributeError, getattr, EvilGetattribute(), "attr")

    def test_type___getattribute__(self):
        self.assertRaises(TypeError, type.__getattribute__, list, type)

    def test_abstractmethods(self):
        # type pretends not to have __abstractmethods__.
        self.assertRaises(AttributeError, getattr, type, "__abstractmethods__")
        class meta(type):
            pass
        self.assertRaises(AttributeError, getattr, meta, "__abstractmethods__")
        class X(object):
            pass
        with self.assertRaises(AttributeError):
            del X.__abstractmethods__

    def test_proxy_call(self):
        class FakeStr(object):
            __class__ = str

        fake_str = FakeStr()
        # isinstance() reads __class__ on new style classes
        self.assertIsInstance(fake_str, str)

        # call a method descriptor
        with self.assertRaises(TypeError):
            str.split(fake_str)

        # call a slot wrapper descriptor
        with self.assertRaises(TypeError):
            str.__add__(fake_str, "abc")

    def test_repr_as_str(self):
        # Issue #11603: crash or infinite loop when rebinding __str__ as
        # __repr__.
        class Foo(object):
            pass
        Foo.__repr__ = Foo.__str__
        foo = Foo()
        self.assertRaises(RuntimeError, str, foo)
        self.assertRaises(RuntimeError, repr, foo)

    def test_mixing_slot_wrappers(self):
        class X(dict):
            __setattr__ = dict.__setitem__
        x = X()
        x.y = 42
        self.assertEqual(x["y"], 42)

    def test_cycle_through_dict(self):
        # See bug #1469629
        class X(dict):
            def __init__(self):
                dict.__init__(self)
                self.__dict__ = self
        x = X()
        x.attr = 42
        wr = weakref.ref(x)
        del x
        test_support.gc_collect()
        self.assertIsNone(wr())
        for o in gc.get_objects():
            self.assertIsNot(type(o), X)

class DictProxyTests(unittest.TestCase):
    def setUp(self):
        class C(object):
            def meth(self):
                pass
        self.C = C

    def test_repr(self):
        self.assertIn('dict_proxy({', repr(vars(self.C)))
        self.assertIn("'meth':", repr(vars(self.C)))

    def test_iter_keys(self):
        # Testing dict-proxy iterkeys...
        keys = [ key for key in self.C.__dict__.iterkeys() ]
        keys.sort()
        self.assertEqual(keys, ['__dict__', '__doc__', '__module__',
            '__weakref__', 'meth'])

    def test_iter_values(self):
        # Testing dict-proxy itervalues...
        values = [ values for values in self.C.__dict__.itervalues() ]
        self.assertEqual(len(values), 5)

    def test_iter_items(self):
        # Testing dict-proxy iteritems...
        keys = [ key for (key, value) in self.C.__dict__.iteritems() ]
        keys.sort()
        self.assertEqual(keys, ['__dict__', '__doc__', '__module__',
            '__weakref__', 'meth'])

    def test_dict_type_with_metaclass(self):
        # Testing type of __dict__ when __metaclass__ set...
        class B(object):
            pass
        class M(type):
            pass
        class C:
            # In 2.3a1, C.__dict__ was a real dict rather than a dict proxy
            __metaclass__ = M
        self.assertEqual(type(C.__dict__), type(B.__dict__))


class PTypesLongInitTest(unittest.TestCase):
    # This is in its own TestCase so that it can be run before any other tests.
    def test_pytype_long_ready(self):
        # Testing SF bug 551412 ...

        # This dumps core when SF bug 551412 isn't fixed --
        # but only when test_descr.py is run separately.
        # (That can't be helped -- as soon as PyType_Ready()
        # is called for PyLong_Type, the bug is gone.)
        class UserLong(object):
            def __pow__(self, *args):
                pass
        try:
            pow(0L, UserLong(), 0L)
        except:
            pass

        # Another segfault only when run early
        # (before PyType_Ready(tuple) is called)
        type.mro(tuple)


def test_main():
    deprecations = [(r'complex divmod\(\), // and % are deprecated$',
                     DeprecationWarning)]
    if sys.py3kwarning:
        deprecations += [
            ("classic (int|long) division", DeprecationWarning),
            ("coerce.. not supported", DeprecationWarning),
            (".+__(get|set|del)slice__ has been removed", DeprecationWarning)]
    with test_support.check_warnings(*deprecations):
        # Run all local test cases, with PTypesLongInitTest first.
        test_support.run_unittest(PTypesLongInitTest, OperatorsTest,
                                  ClassPropertiesAndMethods, DictProxyTests)

if __name__ == "__main__":
    test_main()
