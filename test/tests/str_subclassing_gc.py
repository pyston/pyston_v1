# Make sure that subclasses of str have their memory reclaimed

class S(str):
    pass

def f():
    base = "." * 10000000
    for i in xrange(100):
        s = S(base)
f()

# make sure it has attrs
s = S("blah")
s.blah = 2
print s.blah
