print 'Test getting'

g = {'a': 1}
l = {'b': 2}
exec """global a
print a
print b""" in g, l

print 'Test setting'

g = {}
l = {}
exec """global a
a = 1
b = 2""" in g, l
del g['__builtins__']
print g
print l

print 'Test deleting'

g = {'a': 1}
l = {'b': 2}
exec """global a
del a
del b""" in g, l
del g['__builtins__']
print g
print l

print 'Test global access in a function'

g = {'a': 4, 'b': 5}
l = {}
exec """
def f(i):
    global a, b, c
    print i, 'a =', a
    del b
    c = 20
    for i in xrange(100):
        pass
f(-1)
""" in g, l
# Try to trigger a reopt and osr:
for i in xrange(1000):
    g['b'] = 6
    l['f'](i)
print l['f'].__module__

print 'Test global access in comprehensions'

g = {'a' : 4, 'b': 5, 'c': 6}
exec """
global a
global b
global c
print [a for i in xrange(1)]
print {b for i in xrange(1)}
print {i : b for i in xrange(1)}
print 
""" in g, {}

a = 0
exec "a = 1" in None, None
print a

# Adapted from six.py:
def exec_(_code_, _globs_, _locs_):
    exec("""exec _code_ in _globs_, _locs_""")

g = {'a': 1}
l = {'b': 2}
exec_("""global a
print a
print b""", g, l)


exec """print __name__"""
exec """print __name__""" in {}, {}


# Test classdefs in execs:
b = 3
a = 2
s = """class C(object):
    print "b =", b
    if b:
        c = 2
    else:
        a = -1
    print a, b
print C.__module__, C.__name__, repr(C)
"""
exec s in {'a': 1, 'b': 5}, {'b': 2}
exec s


# Test old-style classdefs in execs:
b = 3
a = 2
s = """class C():
    print "b =", b
    if b:
        c = 2
    else:
        a = -1
    print a, b
print C.__module__
"""
exec s in {'a': 1, 'b': 5}, {'b': 2}
exec s




# test eval+exec in exec:
a = 5
exec """print eval('a')""" in {'a': 6}, {}
exec """exec 'print a' """ in {'a': 6}, {}


# test ordering:
def show(obj, msg):
    print msg
    return obj
exec show("print 'in exec'", "body") in show(None, "globals"), show(None, "locals")


g = {}
l = {}
exec ("a=1; print a", g, l)
print g.keys(), l.keys()
