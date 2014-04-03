# Have to be careful to incref any classes when instances are created;
# we can skip it for builtin classes, but this is an example of why it's required

class C(object):
    pass

c = C()
c.n = 10
C.n2 = 11

C = None
# No direct references to c's class (original 'C') anymore!

# Can still get it with:
print type(c)
print type(c).n2

# Some random stuff to try to elicit errors:
l = []
for i in xrange(10):
    l.append(len(l))
print l

print type(c).n2
