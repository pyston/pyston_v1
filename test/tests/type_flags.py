import slots_test

HEAPTYPE = 1<<9

def test(cls):
    print cls
    slots_test.view_tp_as(cls)
    if HEAPTYPE & cls.__flags__:
        print "heaptype"

def testall(module):
    for n in sorted(dir((module))):
        if n in ("reversed", "AttrwrapperType", "BuiltinMethodType", "BufferType", "DictProxyType", "BuiltinCAPIFunctionType"):
            continue

        cls = getattr(module, n)
        if not isinstance(cls, type):
            continue
        print n
        test(cls)

testall(__builtins__)
import types
testall(types)


class C(object):
    pass
test(C)
