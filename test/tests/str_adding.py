# str has some weird adding behavior that cannot be replicated with Python code.
# Usually, if an __add__ throws an error, then __radd__ isn't tried.
# But string addition is defined in sq_concat, which is tried after __add__ and
# __radd__.  And string addition will throw a TypeError.

class C(object):
    def __add__(self, rhs):
        raise TypeError("dont support add")

class D(object):
    def __radd__(self, lhs):
        return 3.14

print "" + D()

try:
    print C() + D()
except TypeError as e:
    print e
print "" + D()


# This also applies to list:
print [] + D()
