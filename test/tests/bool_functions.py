# Regression test: we rely internally on CompilerType.nonzero() always returning a BOOL,
# in at least these two cases.

def f(x):
    if x:
        pass

    print not x

f(None)
f({})
f([])
f({1})
f(())
f("")
f(0.0)
f(1L)


# Bools are subclasses of ints:

print 1 == True
print 0 == False
print 1 == False
print 0 == True
print True == 0
print type(hash(True))
print hash(0) == hash(False)
print hash(1) == hash(True)
print isinstance(True, int)
print isinstance(False, int)

for lhs in (True, False):
    for rhs in (True, False, 1, 1.0):
        try:
            print lhs.__and__(rhs), lhs.__or__(rhs), lhs.__xor__(rhs)
            print lhs | rhs
            print lhs & rhs
            print lhs ^ rhs
        except Exception as e:
            print e

print range(False, True, True)

print abs(True), abs(False)

d = {}
d[1] = "hello"
d[False] = "world"
print d[True], d[0]
print long(True), float(False)
1.0 + True
