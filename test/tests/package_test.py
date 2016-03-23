import os

import test_package
print 1, test_package.__name__, os.path.normpath(test_package.__file__).replace(".pyc", ".py")

import test_package.intrapackage_import
import test_package.absolute_import
import test_package.absolute_import_with_future
