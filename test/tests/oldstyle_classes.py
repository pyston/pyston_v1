# __bases__ and __name__ not supported yet -- need to add a custom getattr() method for old style classes

class C():
    pass

print C, type(C)
print map(str, C.__bases__), C.__name__
print type(C())

class D(C):
    pass

print D, type(D)
print map(str, D.__bases__), D.__name__
print type(D())

ClassType = type(C)

try:
    ClassType.__new__(int, 1, 1, 1)
except TypeError, e:
    print e

try:
    ClassType.__new__(1, 1, 1, 1)
except TypeError, e:
    print e

print ClassType("test", (), {})
print ClassType("test", (), {"__module__":"fake"})
