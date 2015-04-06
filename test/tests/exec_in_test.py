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
print g
print l

print 'Test deleting'

g = {'a': 1}
l = {'b': 2}
exec """global a
del a
del b""" in g, l
print g
print l

print 'Test global access in a function'

g = {'a': 4, 'b': 5}
l = {}
exec """
def f():
    global a, b, c
    print 'a =', a
    del b
    c = 20
    for i in xrange(1000):
        pass
f()
""" in g, l
# Try to trigger a reopt or osr:
for i in xrange(1000):
    l['f']()

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

