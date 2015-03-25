# PEP-302  import hook test

import sys
from types import ModuleType
import imp

class Loader(object):
    def load_module(self, fullname):
        print "load", fullname
        if fullname.endswith("test"):
            return 1
        else:
            r = ModuleType(fullname)
            r.__package__ = fullname
            r.__path__ = []
            return r

class Finder(object):
    def find_module(self, fullname, path=None):
        print "find", fullname, path
        return Loader()

try:
    import a.b.test
except ImportError, e:
    print "caught import error"
    # The error CPython gives here is "No module named a.b.test".  Both we and PyPy think this
    # is wrong and that the error should be "No module named a".
    # So unfortunately we can't print out the error message.

sys.meta_path.append(Finder())
import a
print a
import a.b.test as test
print test
import a.b as b
print b
