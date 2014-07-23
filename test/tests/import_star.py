# import_target defines __all__ to be ['x']
from import_target import *

print x
try:
    print foo
    assert 0
except NameError:
    pass
try:
    print _x
    assert 0
except NameError:
    pass

# import_nested_target doesn't define __all__
from import_nested_target import *

print y
try:
    print _y
    assert 0
except NameError:
    pass

# import_target_bad_all defines an __all__ with a nonexistent name
try:
    from import_target_bad_all import *
    assert 0
except AttributeError:
    pass

from import_target_custom_all import *
print z
