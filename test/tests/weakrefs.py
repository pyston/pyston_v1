import weakref
import array
import re

# from https://docs.python.org/2/library/weakref.html:
#
# Not all objects can be weakly referenced; those objects which can include class instances, functions written in Python (but not in C), methods (both bound and unbound), sets, frozensets, file objects, generators, type objects, DBcursor objects from the bsddb module, sockets, arrays, deques, regular expression pattern objects, and code objects.
#
# Changed in version 2.4: Added support for files, sockets, arrays, and patterns.
#
# Changed in version 2.7: Added support for thread.lock, threading.Lock, and code objects
#
# Several built-in types such as list and dict do not directly support weak references but can add support through subclassing
#
# CPython implementation detail: Other built-in types such as tuple and long do not support weak references even when subclassed
#

def test_wr(o, extra=None):
    if extra is None:
        extra = type(o)
    try:
        r = weakref.ref(o)
        print "passed", extra
        return r
    except:
        print "failed", extra

def test_subclass_wr(tp):
    class test(tp): pass
    test_wr(test(), "subclass of " + repr(tp))

def test():
    pass

wr = test_wr(test)
print wr() == test
print weakref.getweakrefcount(test)


test_wr(1)
test_wr(1.)
test_wr(1L)
test_wr("hello world")
test_wr([1,2,3])
test_wr((1,2,2))
test_wr(set())
test_wr(frozenset())
test_wr((i*i for i in range(1000000)))
test_wr(set)
test_wr(file("/etc/passwd"))
class Old:
    pass
test_wr(Old)
# missing: db cursor from the bsddb module
# missing: sockets
test_wr(array.array('d', [1.0, 2.0, 3.14]))
test_wr(re.compile('ab*'))
# compile isn't in pyston yet
#test_wr(compile('print "Hello, world"', '<string>', 'exec'))
# missing: thread.lock, threading.Lock

# skip these since we permit them, while cpython doesn't
#test_subclass_wr(long) 
#test_subclass_wr(tuple)
#test_subclass_wr(list)

test_subclass_wr(int)
test_subclass_wr(float)
