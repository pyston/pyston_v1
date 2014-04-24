# expected: fail
# (doesn't collapse '..' in module paths)

# imports test

import sys
print sys.path[0]

import import_target
print import_target.x

import import_target
import_target.foo()

c = import_target.C()

print import_target.import_nested_target.y
import_target.import_nested_target.bar()
d = import_target.import_nested_target.D()

print "testing importfrom:"
from import_target import x as z
print z

import_nested_target = 15
from import_nested_target import y
print "This should still be 15:",import_nested_target
import import_nested_target
print import_nested_target

print import_nested_target.y
import_target.import_nested_target.y = import_nested_target.y + 1
print import_nested_target.y

print z
print y
print __name__
