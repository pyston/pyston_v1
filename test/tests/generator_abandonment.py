print any(i == 5 for i in xrange(10))



# TODO: move this back to nonzero_exceptions when it's working again:
class MyException(Exception):
    pass
class C(object):
    def __init__(self, x):
        self.x = x
    def __nonzero__(self):
        raise MyException(self.x)
    def __repr__(self):
        return "<C %r>" % self.x
try:
    print list(1 for i in range(5) if C(7))
except MyException, e:
    print e
