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
exec """
def f():
    global a, b, c
    print 'a =', a
    del b
    c = 20
f()
""" in g, {}
