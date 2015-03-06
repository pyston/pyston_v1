import sys, os

# this zip contains two pkgs: test1 and test2
zipdir = os.path.dirname(os.path.realpath(__file__))
filename = zipdir + "/zipimport_test_file.zip"

sys.path.append(filename)
import test1
import test2

