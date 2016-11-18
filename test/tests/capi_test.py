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
