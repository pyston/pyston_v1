# o() and o.__call__() are not always equivalent due to the possibility of __getattribute__ or instance attributes:

def f():
    print "func"

class C(object):
    def __getattribute__(self, attr):
        print "__getattribute__", attr
        if attr == "__call__":
            return f
        return object.__getattribute__(self, attr)

    def __call__(self):
        print "__call__"

c = C()
c()
c.__call__()

print type
print type(1)

# As evidenced by the following real + important example:
# type() is a special function that gets the type of an object:
print type(C) is type
# but type.__call__() is the clsattr for any (non-metaclassed) class,
# which is how object creation gets handled:
print type.__call__(C) is type
