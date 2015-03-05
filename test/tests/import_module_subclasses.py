
# The six.py module exposed some issues with how we were handling imports;
# an imported name is not necessarily an attribute of the module object itself,
# it can be a class object too.  It also goes through descriptor logic.
#
# This test is an attempt to extract some of the behavior of the six.py module
# for testing.

import sys

class ImportHook(object):
    def find_module(self, name, path=None):
        print "find_module('%s')" % name, path
        if name.startswith("pseudo"):
            return self
        return None

    def load_module(self, name):
        return MyModule(name)

class MyDescr(object):
    def __get__(self, *args):
        print "descr.get", args
        return self

    def __repr__(self):
        return "<MyDescr>"

import types
class MyModule(types.ModuleType):
    # This __path__ is on the class type but should still get found:
    __path__ = []

    # Fetching this object will invoke a descriptor
    descr = MyDescr()

    def __init__(self, name):
        super(MyModule, self).__init__(name)
        self.inst_descr = MyDescr()

sys.meta_path.append(ImportHook())
import pseudo
print pseudo

import pseudo.a.b as b
print b
from pseudo.c import c
print c
from pseudo.e import descr, inst_descr
print descr, inst_descr
import pseudo.f.descr as d
print d
import pseudo.g.inst_descr as d
print d

# Other things to test: __all__, __file__, __name__, __package__

