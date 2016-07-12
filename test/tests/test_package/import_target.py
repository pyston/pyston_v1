print "running test_package.import_target"

# Since we are currently importing test_package.import_target, this
# import will succeed (return directly from sys.modules), even though
# test_package will not have the 'import_target' attribute yet

import test_package.import_target

try:
    print test_package.import_target
    assert 0
except AttributeError:
    pass

try:
    print getattr(test_package, 'import_target')
    assert 0
except AttributeError:
    pass

# You can do 'import test_package.import_target', but adding an asname will cause an exception:
try:
    import test_package.import_target as i
    assert 0
    i
except AttributeError:
    pass
