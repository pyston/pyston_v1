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
