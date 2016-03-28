import os
import zipimport

# this zip contains two pkgs: test1 and test2
zipdir = os.path.dirname(os.path.realpath(__file__))
filename = zipdir + "/zipimport_test_file.zip"

z = zipimport.zipimporter(filename)
for name in ("test1", "test2", "test3"):
    try:
        z.is_package(name)
        print z.archive
        print z.prefix
        m = z.load_module(name)
        print m.__file__
        print m.__name__
    except Exception as e:
        print e

