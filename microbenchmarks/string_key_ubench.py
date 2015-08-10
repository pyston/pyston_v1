# Test the speed of using strings as dictionary keys.
# We internally optimize many things to not use dictionaries,
# but there are some times that we can't.

def f():
    d = {'a':1}
    for i in xrange(3000000):
        d['a']
        d['a']
        d['a']
        d['a']
        d['a']
        d['a']
        d['a']
        d['a']
        d['a']
        d['a']
f()
