from __future__ import absolute_import
import os
import import_target

print 4, import_target.__name__, os.path.normpath(import_target.__file__).replace(".pyc", ".py")
