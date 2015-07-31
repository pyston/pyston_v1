# skip-if: True
# expected: fail
# - CPython calls subclasscheck twice, while we call it once.
#   Looks like this is because CPython calls PyErr_NormalizeException
#   when the exception gets set.

class M(type):
    def __subclasscheck__(self, sub):
        print "subclasscheck", sub
        return True

class E(Exception):
    __metaclass__ = M

# This calls __subclasscheck__ twice...?
try:
    raise E()
except E:
    pass
