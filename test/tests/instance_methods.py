class C(object):
    def foo(self):
        pass

print type(C.foo)
print type(C.foo.im_func), type(C.foo.__func__)
print type(C.foo.im_self), type(C.foo.__self__)
