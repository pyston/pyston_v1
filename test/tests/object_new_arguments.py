# should_error
# skip-if: sys.version_info < (2, 7, 4)
# - Error message changed in 2.7.4

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
object.__new__(C2, 1)
