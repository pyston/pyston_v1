# expected: fail
# - packages not supported
# - intra-package imports ("from . import foo") not supported
# - "from __future__ import absolute_import" not supported

import test_package
print test_package

import test_package.intrapackage_import
import test_package.absolute_import
import test_package.absolute_import_with_future
