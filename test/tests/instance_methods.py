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

C().foo.__call__()


# Check comparisons and hashing:
class C(object):
    def foo(self):
        pass

assert C.foo is not C.foo # This could be fine, but if it's true then the rest of the checks here don't make sense
assert C.foo == C.foo
assert not (C.foo != C.foo)
assert not (C.foo < C.foo)
assert not (C.foo > C.foo)
assert C.foo >= C.foo
assert C.foo <= C.foo
assert len({C.foo, C.foo}) == 1

c = C()
assert c.foo is not c.foo # This could be fine, but if it's true then the rest of the checks here don't make sense
assert c.foo == c.foo
assert not (c.foo != c.foo)
assert not (c.foo < c.foo)
assert not (c.foo > c.foo)
assert c.foo >= c.foo
assert c.foo <= c.foo
assert len({c.foo, c.foo}) == 1
