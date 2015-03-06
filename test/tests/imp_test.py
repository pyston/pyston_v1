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

