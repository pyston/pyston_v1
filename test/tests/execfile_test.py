# expected: fail
# - we throw some very weird error here

try:
    execfile("doesnt_exist.py")
except IOError, e:
    print e

import os
fn = os.path.join(os.path.dirname(__file__), 'execfile_target.py')
execfile(fn)
print "done with first execfile"
execfile(fn)

print test_name
print type(execfile_target)
