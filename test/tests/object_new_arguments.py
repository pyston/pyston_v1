# object.__new__ doesn't complain if __init__ is overridden:

class C1(object):
    def __init__(self, a):
        pass

class C2(object):
    pass

print "Trying C1"
object.__new__(C1, 1)
object.__new__(C1, a=1)

print "Trying C2"
try:
    object.__new__(C2, 1)
except TypeError as e:
    print "caught TypeError"

# These are some tricky cases, since they can potentially look like arguments
# are being passed, but really they are not.
type.__call__(*[C2])
type.__call__(C2, **{})

try:
    type.__call__(*[])
except TypeError as e:
    print "caught typeerror"
