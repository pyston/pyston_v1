# expected: fail
# (del statement)

# Not sure how important or necessary this behavior is, but
# CPython's behavior around sys.modules seems to be that it saves
# an internal reference to the object, so if you set sys.modules
# to something else, importing will still work.  If you modify sys.modules,
# though, that affects importing.
# PyPy seems to not do this, and setting sys.modules to something else
# raises an exception when you try to import something else.
# Jython seems to crash.

import sys

print 'sys' in sys.modules
print 'import_target' in sys.modules

import import_target
import import_target

print 'import_target' in sys.modules
del sys.modules['import_target']

import import_target
import import_target

sys_modules = sys.modules
print "setting sys.modules=0"
sys.modules = 0

import import_target

print "clearing old sys.modules"
sys_modules.clear()
import import_target
