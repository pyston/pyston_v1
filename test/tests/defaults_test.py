# Test for various defaults arguments in builtin functions:

class ExpectationFailedException(Exception):
    pass

class ExpectedException(object):
    def __init__(self, excs):
        if isinstance(excs, BaseException):
            excs = (excs,)
        self.excs = excs

    def __enter__(self):
        pass

    def __exit__(self, type, val, tback):
        if not val:
            raise ExpectationFailedException("Didn't raise any exception")
        if not isinstance(val, self.excs):
            raise ExpectationFailedException("Raised %s instead of %s" % (val, self.excs))
        print "Caught", type.__name__
        return True
expected_exception = ExpectedException

d = {}
print d.get(1)
print d.setdefault(2)
print d.pop(2)
print d.pop(2, None)
print d.pop(2, None)
with expected_exception(KeyError):
    print d.pop(2)

print min([1])
print min([1], None)

with expected_exception(AttributeError):
    print getattr(object(), "")
print getattr(object(), "", None)

print range(5)
with expected_exception(TypeError):
    print range(5, None)
print range(5, 10)

print list(xrange(5))
with expected_exception(TypeError):
    print list(xrange(5, None))
print list(xrange(5, 10))
