import api_test
print("Hello")

class A(object):
    def foo(self):
        print("Hello from A!")

class B(object):
    def foo(self):
        print(type(self))
        print("Hello from B!")

a = A()
b = B()
foo = b.foo
foo()
api_test.change_self(foo, a)
foo()

try:
    import __pyston__
    class A(object):
        def __init__(self):
            self.name = 'Pyston'
    a = A()
    # this C function will test three things:
    # - Use PyObject_GetDict to get object's dict and return the obj.name
    # - Use PyObjcet_ClearDict to clear object's dict.
    # - Use PyObject_SetDict to add {'value': 42} to object's dict.
    ret, stub = api_test.dict_API_test(a)
    assert(ret == 'Pyston')
    assert(a.__dict__['value'] == 42)
except ImportError:
    pass
