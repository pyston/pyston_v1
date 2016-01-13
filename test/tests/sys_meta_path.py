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

class Finder2(object):
    def __new__(cls, path):
        print "new2", path
        return object.__new__(cls)

    def __init__(self, path):
        self.path = path

    def find_module(self, fullname):
        print "find2", self.path, fullname
        return None

# Fill the importer cache, so that we don't have to worry about the exact
# sys.path:
try:
    import a.b.test
except ImportError, e:
    # The error CPython gives here is "No module named a.b.test".  Both we and PyPy think this
    # is wrong and that the error should be "No module named a".
    # So unfortunately we can't print out the error message.
    print "caught import error 1"

def ImportErrorPathHook(a):
    print "ImportErrorPathHook", a
    raise ImportError

sys.path_hooks.insert(0, ImportErrorPathHook)
sys.path_hooks.append(Finder2)
try:
    import a.b.test
except ImportError, e:
    print "caught import error 2"

# this should not call into ImportErrorPathHook
try:
    imp.find_module("foobar", ["/"])
except ImportError, e:
    print "caught import error 3"

sys.path.append("/my_magic_directory")

try:
    import a.b.test
except ImportError, e:
    print "caught import error 4"

sys.meta_path.append(Finder())
import a
print a
import a.b.test as test
print test
import a.b as b
print b

class RecursiveLoader(object):
    def find_module(self, name, path=None):
        print "RecursiveLoader", name, path
        imp.find_module(name, path)
        return Loader()
sys.meta_path.insert(0, RecursiveLoader())
import subprocess
