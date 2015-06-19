# expected: fail
import unittest
from test.test_support import check_syntax_error, check_py3k_warnings, \
                              check_warnings, run_unittest


class ScopeTests(unittest.TestCase):

    def testSimpleNesting(self):

        def make_adder(x):
            def adder(y):
                return x + y
            return adder

        inc = make_adder(1)
        plus10 = make_adder(10)

        self.assertEqual(inc(1), 2)
        self.assertEqual(plus10(-2), 8)

    def testExtraNesting(self):

        def make_adder2(x):
            def extra(): # check freevars passing through non-use scopes
                def adder(y):
                    return x + y
                return adder
            return extra()

        inc = make_adder2(1)
        plus10 = make_adder2(10)

        self.assertEqual(inc(1), 2)
        self.assertEqual(plus10(-2), 8)

    def testSimpleAndRebinding(self):

        def make_adder3(x):
            def adder(y):
                return x + y
            x = x + 1 # check tracking of assignment to x in defining scope
            return adder

        inc = make_adder3(0)
        plus10 = make_adder3(9)

        self.assertEqual(inc(1), 2)
        self.assertEqual(plus10(-2), 8)

    def testNestingGlobalNoFree(self):

        def make_adder4(): # XXX add exta level of indirection
            def nest():
                def nest():
                    def adder(y):
                        return global_x + y # check that plain old globals work
                    return adder
                return nest()
            return nest()

        global_x = 1
        adder = make_adder4()
        self.assertEqual(adder(1), 2)

        global_x = 10
        self.assertEqual(adder(-2), 8)

    def testNestingThroughClass(self):

        def make_adder5(x):
            class Adder:
                def __call__(self, y):
                    return x + y
            return Adder()

        inc = make_adder5(1)
        plus10 = make_adder5(10)

        self.assertEqual(inc(1), 2)
        self.assertEqual(plus10(-2), 8)

    def testNestingPlusFreeRefToGlobal(self):

        def make_adder6(x):
            global global_nest_x
            def adder(y):
                return global_nest_x + y
            global_nest_x = x
            return adder

        inc = make_adder6(1)
        plus10 = make_adder6(10)

        self.assertEqual(inc(1), 11) # there's only one global
        self.assertEqual(plus10(-2), 8)

    def testNearestEnclosingScope(self):

        def f(x):
            def g(y):
                x = 42 # check that this masks binding in f()
                def h(z):
                    return x + z
                return h
            return g(2)

        test_func = f(10)
        self.assertEqual(test_func(5), 47)

    def testMixedFreevarsAndCellvars(self):

        def identity(x):
            return x

        def f(x, y, z):
            def g(a, b, c):
                a = a + x # 3
                def h():
                    # z * (4 + 9)
                    # 3 * 13
                    return identity(z * (b + y))
                y = c + z # 9
                return h
            return g

        g = f(1, 2, 3)
        h = g(2, 4, 6)
        self.assertEqual(h(), 39)

    def testFreeVarInMethod(self):

        def test():
            method_and_var = "var"
            class Test:
                def method_and_var(self):
                    return "method"
                def test(self):
                    return method_and_var
                def actual_global(self):
                    return str("global")
                def str(self):
                    return str(self)
            return Test()

        t = test()
        self.assertEqual(t.test(), "var")
        self.assertEqual(t.method_and_var(), "method")
        self.assertEqual(t.actual_global(), "global")

        method_and_var = "var"
        class Test:
            # this class is not nested, so the rules are different
            def method_and_var(self):
                return "method"
            def test(self):
                return method_and_var
            def actual_global(self):
                return str("global")
            def str(self):
                return str(self)

        t = Test()
        self.assertEqual(t.test(), "var")
        self.assertEqual(t.method_and_var(), "method")
        self.assertEqual(t.actual_global(), "global")

    def testRecursion(self):

        def f(x):
            def fact(n):
                if n == 0:
                    return 1
                else:
                    return n * fact(n - 1)
            if x >= 0:
                return fact(x)
            else:
                raise ValueError, "x must be >= 0"

        self.assertEqual(f(6), 720)


    def testUnoptimizedNamespaces(self):

        check_syntax_error(self, """\
def unoptimized_clash1(strip):
    def f(s):
        from string import *
        return strip(s) # ambiguity: free or local
    return f
""")

        check_syntax_error(self, """\
def unoptimized_clash2():
    from string import *
    def f(s):
        return strip(s) # ambiguity: global or local
    return f
""")

        check_syntax_error(self, """\
def unoptimized_clash2():
    from string import *
    def g():
        def f(s):
            return strip(s) # ambiguity: global or local
        return f
""")

        # XXX could allow this for exec with const argument, but what's the point
        check_syntax_error(self, """\
def error(y):
    exec "a = 1"
    def f(x):
        return x + y
    return f
""")

        check_syntax_error(self, """\
def f(x):
    def g():
        return x
    del x # can't del name
""")

        check_syntax_error(self, """\
def f():
    def g():
        from string import *
        return strip # global or local?
""")

        # and verify a few cases that should work

        exec """
def noproblem1():
    from string import *
    f = lambda x:x

def noproblem2():
    from string import *
    def f(x):
        return x + 1

def noproblem3():
    from string import *
    def f(x):
        global y
        y = x
"""

    def testLambdas(self):

        f1 = lambda x: lambda y: x + y
        inc = f1(1)
        plus10 = f1(10)
        self.assertEqual(inc(1), 2)
        self.assertEqual(plus10(5), 15)

        f2 = lambda x: (lambda : lambda y: x + y)()
        inc = f2(1)
        plus10 = f2(10)
        self.assertEqual(inc(1), 2)
        self.assertEqual(plus10(5), 15)

        f3 = lambda x: lambda y: global_x + y
        global_x = 1
        inc = f3(None)
        self.assertEqual(inc(2), 3)

        f8 = lambda x, y, z: lambda a, b, c: lambda : z * (b + y)
        g = f8(1, 2, 3)
        h = g(2, 4, 6)
        self.assertEqual(h(), 18)

    def testUnboundLocal(self):

        def errorInOuter():
            print y
            def inner():
                return y
            y = 1

        def errorInInner():
            def inner():
                return y
            inner()
            y = 1

        self.assertRaises(UnboundLocalError, errorInOuter)
        self.assertRaises(NameError, errorInInner)

        # test for bug #1501934: incorrect LOAD/STORE_GLOBAL generation
        exec """
global_x = 1
def f():
    global_x += 1
try:
    f()
except UnboundLocalError:
    pass
else:
    fail('scope of global_x not correctly determined')
""" in {'fail': self.fail}

    def testComplexDefinitions(self):

        def makeReturner(*lst):
            def returner():
                return lst
            return returner

        self.assertEqual(makeReturner(1,2,3)(), (1,2,3))

        def makeReturner2(**kwargs):
            def returner():
                return kwargs
            return returner

        self.assertEqual(makeReturner2(a=11)()['a'], 11)

        with check_py3k_warnings(("tuple parameter unpacking has been removed",
                                  SyntaxWarning)):
            exec """\
def makeAddPair((a, b)):
    def addPair((c, d)):
        return (a + c, b + d)
    return addPair
""" in locals()
        self.assertEqual(makeAddPair((1, 2))((100, 200)), (101,202))

    def testScopeOfGlobalStmt(self):
# Examples posted by Samuele Pedroni to python-dev on 3/1/2001

        exec """\
# I
x = 7
def f():
    x = 1
    def g():
        global x
        def i():
            def h():
                return x
            return h()
        return i()
    return g()
self.assertEqual(f(), 7)
self.assertEqual(x, 7)

# II
x = 7
def f():
    x = 1
    def g():
        x = 2
        def i():
            def h():
                return x
            return h()
        return i()
    return g()
self.assertEqual(f(), 2)
self.assertEqual(x, 7)

# III
x = 7
def f():
    x = 1
    def g():
        global x
        x = 2
        def i():
            def h():
                return x
            return h()
        return i()
    return g()
self.assertEqual(f(), 2)
self.assertEqual(x, 2)

# IV
x = 7
def f():
    x = 3
    def g():
        global x
        x = 2
        def i():
            def h():
                return x
            return h()
        return i()
    return g()
self.assertEqual(f(), 2)
self.assertEqual(x, 2)

# XXX what about global statements in class blocks?
# do they affect methods?

x = 12
class Global:
    global x
    x = 13
    def set(self, val):
        x = val
    def get(self):
        return x

g = Global()
self.assertEqual(g.get(), 13)
g.set(15)
self.assertEqual(g.get(), 13)
"""

    def testLeaks(self):

        class Foo:
            count = 0

            def __init__(self):
                Foo.count += 1

            def __del__(self):
                Foo.count -= 1

        def f1():
            x = Foo()
            def f2():
                return x
            f2()

        for i in range(100):
            f1()

        self.assertEqual(Foo.count, 0)

    def testClassAndGlobal(self):

        exec """\
def test(x):
    class Foo:
        global x
        def __call__(self, y):
            return x + y
    return Foo()

x = 0
self.assertEqual(test(6)(2), 8)
x = -1
self.assertEqual(test(3)(2), 5)

looked_up_by_load_name = False
class X:
    # Implicit globals inside classes are be looked up by LOAD_NAME, not
    # LOAD_GLOBAL.
    locals()['looked_up_by_load_name'] = True
    passed = looked_up_by_load_name

self.assertTrue(X.passed)
"""

    def testLocalsFunction(self):

        def f(x):
            def g(y):
                def h(z):
                    return y + z
                w = x + y
                y += 3
                return locals()
            return g

        d = f(2)(4)
        self.assertIn('h', d)
        del d['h']
        self.assertEqual(d, {'x': 2, 'y': 7, 'w': 6})

    def testLocalsClass(self):
        # This test verifies that calling locals() does not pollute
        # the local namespace of the class with free variables.  Old
        # versions of Python had a bug, where a free variable being
        # passed through a class namespace would be inserted into
        # locals() by locals() or exec or a trace function.
        #
        # The real bug lies in frame code that copies variables
        # between fast locals and the locals dict, e.g. when executing
        # a trace function.

        def f(x):
            class C:
                x = 12
                def m(self):
                    return x
                locals()
            return C

        self.assertEqual(f(1).x, 12)

        def f(x):
            class C:
                y = x
                def m(self):
                    return x
                z = list(locals())
            return C

        varnames = f(1).z
        self.assertNotIn("x", varnames)
        self.assertIn("y", varnames)

    def testLocalsClass_WithTrace(self):
        # Issue23728: after the trace function returns, the locals()
        # dictionary is used to update all variables, this used to
        # include free variables. But in class statements, free
        # variables are not inserted...
        import sys
        sys.settrace(lambda a,b,c:None)
        try:
            x = 12

            class C:
                def f(self):
                    return x

            self.assertEqual(x, 12) # Used to raise UnboundLocalError
        finally:
            sys.settrace(None)

    def testBoundAndFree(self):
        # var is bound and free in class

        def f(x):
            class C:
                def m(self):
                    return x
                a = x
            return C

        inst = f(3)()
        self.assertEqual(inst.a, inst.m())

    def testInteractionWithTraceFunc(self):

        import sys
        def tracer(a,b,c):
            return tracer

        def adaptgetter(name, klass, getter):
            kind, des = getter
            if kind == 1:       # AV happens when stepping from this line to next
                if des == "":
                    des = "_%s__%s" % (klass.__name__, name)
                return lambda obj: getattr(obj, des)

        class TestClass:
            pass

        sys.settrace(tracer)
        adaptgetter("foo", TestClass, (1, ""))
        sys.settrace(None)

        self.assertRaises(TypeError, sys.settrace)

    def testEvalExecFreeVars(self):

        def f(x):
            return lambda: x + 1

        g = f(3)
        self.assertRaises(TypeError, eval, g.func_code)

        try:
            exec g.func_code in {}
        except TypeError:
            pass
        else:
            self.fail("exec should have failed, because code contained free vars")

    def testListCompLocalVars(self):

        try:
            print bad
        except NameError:
            pass
        else:
            print "bad should not be defined"

        def x():
            [bad for s in 'a b' for bad in s.split()]

        x()
        try:
            print bad
        except NameError:
            pass

    def testEvalFreeVars(self):

        def f(x):
            def g():
                x
                eval("x + 1")
            return g

        f(4)()

    def testFreeingCell(self):
        # Test what happens when a finalizer accesses
        # the cell where the object was stored.
        class Special:
            def __del__(self):
                nestedcell_get()

        def f():
            global nestedcell_get
            def nestedcell_get():
                return c

            c = (Special(),)
            c = 2

        f() # used to crash the interpreter...

    def testGlobalInParallelNestedFunctions(self):
        # A symbol table bug leaked the global statement from one
        # function to other nested functions in the same block.
        # This test verifies that a global statement in the first
        # function does not affect the second function.
        CODE = """def f():
    y = 1
    def g():
        global y
        return y
    def h():
        return y + 1
    return g, h

y = 9
g, h = f()
result9 = g()
result2 = h()
"""
        local_ns = {}
        global_ns = {}
        exec CODE in local_ns, global_ns
        self.assertEqual(2, global_ns["result2"])
        self.assertEqual(9, global_ns["result9"])

    def testTopIsNotSignificant(self):
        # See #9997.
        def top(a):
            pass
        def b():
            global a


def test_main():
    with check_warnings(("import \* only allowed at module level",
                         SyntaxWarning)):
        run_unittest(ScopeTests)

if __name__ == '__main__':
    test_main()
