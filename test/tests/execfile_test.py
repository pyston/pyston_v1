# expected: fail
# execfile() not implemented yet

try:
    execfile("doesnt_exist.py")
except IOError, e:
    print e

import os
fn = os.path.join(os.path.dirname(__file__), 'execfile_target.py')
execfile(fn)
print "done with first execfile"
execfile(fn)
