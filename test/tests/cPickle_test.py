import cPickle

l = [[], (123,)]
l.append(l)
s = cPickle.dumps(l)
print repr(s)

l2 = cPickle.loads(s)
l3 = l2.pop()
print l2, l3, l2 is l3

print cPickle.loads(cPickle.dumps("hello world"))

# Sqlalchemy wants this:
import operator
print repr(cPickle.dumps(len))
print repr(cPickle.dumps(operator.and_))

class C(object):
    pass
c = C()
c.a = 1
# print repr(cPickle.dumps(c)) # Our output is different to cpythons because we don't do some refcounting opts.
print cPickle.loads(cPickle.dumps(c)).a


for obj in [(1, 2), "hello world", u"hola world", 1.0, 1j, 1L, 2, {1:2}, set([3]), frozenset([4])]:
    print
    print "Testing pickling subclasses of %s..." % type(obj)

    class MySubclass(type(obj)):
        pass

    o = MySubclass(obj)
    print repr(o), type(o)

    for protocol in (0, 1, 2):
        print "Protocol", protocol

        s = cPickle.dumps(o, protocol=protocol)
        # print repr(s) # Our output is different to cpythons because we don't do some refcounting opts.

        o2 = cPickle.loads(s)
        print repr(o2), type(o2)

import cStringIO
StringIO = cPickle.loads(cPickle.dumps(cStringIO.StringIO))
print type(StringIO())

import thread
print cPickle.loads(cPickle.dumps(thread.error))
