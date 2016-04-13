import re
import inspect
def gen():
  print ("generator test")
  yield 'a'
  yield 'b'
 
def is_subclassable(base_cls):
  try:
    class C(base_cls):
      pass

  except TypeError as e:
    assert 'is not an acceptable base type' in repr(e)
    return False
  return True

class b:
  def __init__(self):
    self.c = 1
  def d(self):
    print self.c

def test(cls):
    print cls.__name__, is_subclassable(cls)

def testall(module):
    for n in sorted(dir((module))):
        if n in ("reversed", "AttrwrapperType", "BuiltinMethodType", "BufferType", "DictProxyType", "BuiltinCAPIFunctionType"):
            continue

        cls = getattr(module, n)
        if not isinstance(cls, type):
            continue
        test(cls)

import types
testall(types)
testall(__builtins__)

#slice
assert not is_subclassable(slice)
#xrange
assert not is_subclassable(type(xrange(1)))
#range-iterator
assert not is_subclassable(type(iter(range(5))))
#xrange-iterator
assert not is_subclassable(type(iter(xrange(5))))
#callable-iterator
assert not is_subclassable(type(re.finditer(r'\bs\w+', "some text with swords")))
#striterator
assert not is_subclassable(type(iter("abc")))
#memoryview
assert not is_subclassable(type(memoryview("abc")))
#buffer
assert not is_subclassable(type(buffer('abcde', 2,1)))
#Ellipsis
assert not is_subclassable(type(Ellipsis))
#generator
assert not is_subclassable(type(gen()))
#bool
assert not is_subclassable(type(True))
#classobj
assert not is_subclassable(type(b))
#code
assert not is_subclassable(type(compile('sum([1, 2, 3])', '', 'single')))
#instance
ins = b()
assert not is_subclassable(type(ins))
#instance_method
assert not is_subclassable(type(ins.d))
#frame
assert not is_subclassable(type(inspect.currentframe()))
#function
assert not is_subclassable(type(is_subclassable))

test(type(iter([])))
test(type(reversed([])))
test(type(reversed(()))) # not the same as reversed([])
test(type(iter(set())))
# test(type(iter("")))
test(type(iter({})))
test(type(iter(())))
test(type({}.iterkeys()))
test(type(enumerate([])))
import sys
test(type(sys.flags))
test(super)
test(type(None))
