import imp
print len(imp.find_module("os"))
e = imp.find_module("encodings")
print e[0]
m = imp.load_module("myenc", e[0], e[1], e[2])
print m.__name__

# Null Importer tests
for a in (1, "", "/proc", "nonexisting_dir"):
    try:
        i = imp.NullImporter(a)
        print i.find_module("foo")
    except Exception as e:
        print e

imp.acquire_lock()
imp.release_lock()

import os
print "first load_source():"
m1 = imp.load_source("import_target", os.path.join(os.path.dirname(__file__), "import_target.py"))
print "second load_source():"
m2 = imp.load_source("import_target", os.path.join(os.path.dirname(__file__), "import_target.py"))
print m1 is m2

m = imp.new_module("My new module")
print type(m), m, hasattr(m, "__file__")
print imp.is_builtin("sys"), imp.is_frozen("sys")
print imp.is_builtin("io"), imp.is_frozen("io")

e = imp.find_module("1")
m = imp.load_module("test_1", e[0], e[1], e[2])

def n(s):
    return str(s).replace(".pyc", ".py")

print n(m), n(m.__name__), n(m.__file__), hasattr(m, "__path__")

import sys, types
name = "json"
m = sys.modules[name] = types.ModuleType(name)
print sorted(dir(m))
s = imp.find_module(name)
m = imp.load_module(name, *s)
print name in m.__file__, sorted(dir(m))
