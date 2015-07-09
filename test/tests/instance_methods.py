class C(object):
    def foo(self):
        pass

    def __repr__(self):
        return 'some C obj'

print type(C.foo)
print type(C.foo.im_func), type(C.foo.__func__)
print type(C.foo.im_self), type(C.foo.__self__)
print type(C.foo.im_class)
print repr(C.foo)

print type(C().foo)
print type(C().foo.im_func), type(C().foo.__func__)
print type(C().foo.im_self), type(C().foo.__self__)
print type(C().foo.im_class)
print repr(C().foo)

# old-style classes

class C:
    def foo(self):
        pass

    def __repr__(self):
        return 'some old-style C obj'

print type(C.foo)
print type(C.foo.im_func), type(C.foo.__func__)
print type(C.foo.im_self), type(C.foo.__self__)
print type(C.foo.im_class)
print repr(C.foo)

print type(C().foo)
print type(C().foo.im_func), type(C().foo.__func__)
print type(C().foo.im_self), type(C().foo.__self__)
print type(C().foo.im_class)
print repr(C().foo)

def f(m):
    print m.__name__

f(C.foo)
f(C().foo)
