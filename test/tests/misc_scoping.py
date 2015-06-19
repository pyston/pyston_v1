# expected: fail

def f():
    print eval("[a for a in xrange(2)]")
    print eval("a")
f()

def f():
    a = 0
    b = 0
    e = 0
    r = 0
    def g():
        def h():
            print b
            print e
        print a

        c = 0
        print c

        eval("[a for a in xrange(2)]")
        eval("[c for c in xrange(2)]")
        eval("[d for d in xrange(2)]")
        eval("[e for e in xrange(2)]")

        print a
        print c

        # d not found, because it's read as a stack variable
        try:
            print d
        except NameError:
            print 'd not found'

        # but d it *is* in the locals()
        # On the other hand, a, c, and e don't get over-written
        # and b appears even though it only gets passed-through.
        # So it should look like:
        # a:0, b:0, c:0, d:2, e:0
        print locals()

    def unused():
        print r
    g()
f()

def meh(l):
    l['a'] = 5
    return 3

def f():
    print eval("meh(locals()) + a")
f()

def f():
    print eval("meh(locals()) + a", globals(), {})
f()

def f():
    d = locals()
    a = 2
    d['a'] = 3
    print a
    print d
f()

def f():
    exec "print 'hi'"
    d = locals()
    a = 2
    d['a'] = 3
    print a
    print d
f()

def f():
    d = locals()
    a = 2
    d['a'] = 3
    print a
    print d
    exec "print 'hi'"
f()

def f(arg):
    a = 2
    d = locals()
    print d
    a = 3
    print d
    locals()
    print d
    del a
    print d
    locals()
    print d
f(12)

def f(arg):
    exec "r = 12"
    a = 2
    d = locals()
    print d
    a = 3
    print d
    locals()
    print d
    del a
    print d
    locals()
    print d
f(12)


def f():
    a = 5
    def g():
        print a
    print locals()
f()

def f():
    def g():
        a = 0
        def h():
            print a
            print locals()
            yield 12
            print locals()
            yield 13
        yield h
        a = 1
        yield 2
    gen = g()
    h1 = gen.next()
    hgen = h1()

    hgen.next()
    gen.next()
    hgen.next()
f()

foo = 0 
class C(object):
    try:
        del foo
    except NameError:
        print 'foo NameError'

foo = 0
class C(object):
    foo = 1
    print foo
    del foo
    print foo

class C(object):
    a = 2
    d = locals()
    print d
    a = 3
    print d
    locals()
    print d
    del a
    print d

def f(moo):
    class C(object):
        a = 2
        d = locals()
        print d
        a = 3
        print d
        locals()
        print d
        del a
        print d
        print moo
f(2134)

some_glob = 2
def f():
    global some_glob
    def g():
        exec "some_glob = 5"
        print some_glob
    g()
f()

some_glob = 2
def f():
    def g():
        global some_glob
        exec "some_glob = 5"
        print some_glob
    g()
f()

some_glob = 2
def f():
    global some_glob
    exec "some_glob = 5"
    def g():
        print some_glob
    g()
f()
