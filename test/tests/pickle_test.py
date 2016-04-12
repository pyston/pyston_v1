import pickle

l = [[], (123,)]
l.append(l)
s = pickle.dumps(l)
print repr(s)

l2 = pickle.loads(s)
l3 = l2.pop()
print l2, l3, l2 is l3

print pickle.loads(pickle.dumps("hello world"))

# Sqlalchemy wants this:
import operator
print repr(pickle.dumps(len))
print repr(pickle.dumps(operator.and_))

class C(object):
    pass
c = C()
c.a = 1
print repr(pickle.dumps(c))
print pickle.loads(pickle.dumps(c)).a


for obj in [(1, 2), "hello world", u"hola world", 1.0, 1j, 1L, 2, {1:2}, set([3]), frozenset([4])]:
    print
    print "Testing pickling subclasses of %s..." % type(obj)

    class MySubclass(type(obj)):
        pass

    o = MySubclass(obj)
    print repr(o), type(o)

    for protocol in (0, 1, 2):
        print "Protocol", protocol

        s = pickle.dumps(o, protocol=protocol)
        print repr(s)

        o2 = pickle.loads(s)
        print repr(o2), type(o2)

import cStringIO
StringIO = pickle.loads(pickle.dumps(cStringIO.StringIO))
print type(StringIO())
