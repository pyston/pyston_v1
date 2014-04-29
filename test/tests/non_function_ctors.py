# expected: fail
# - not implemented yet

class CallableNew(object):
    def __call__(self, cls, arg):
        print "new", cls, arg
        return object.__new__(cls)
class CallableInit(object):
    def __call__(self, arg):
        print "init", arg
class C(object):
    def __getattribute__(self, name):
        # This shouldn't get called
        print "__getattribute__", name

C.__new__ = CallableNew()
C.__init__ = CallableInit()

c = C(1)
print c
