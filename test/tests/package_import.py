import distutils
import distutils.log
print type(distutils)
print type(distutils.log)

import sys
print distutils.log == sys.modules['distutils.log']
print 'log' in sys.modules
print distutils.log.__name__ # should be "distutils.log"

try:
    import distutils.doesnt_exist
except ImportError, e:
    # CPython prints: "No module named doesnt_exist"
    # PyPy prints: "No module named distutils.doesnt_exist"
    # We print "No module named doesnt_exist"
    print e

try:
    import distutils7.doesnt_exist
except ImportError, e:
    # CPython prints: "No module named distutils7.doesnt_exist"
    # PyPy prints: "No module named distutils7"
    # We print: "No module named distutils7"
    # print e
    print "(Caught import error)"
