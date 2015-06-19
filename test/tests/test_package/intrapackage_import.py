import os
from . import import_target
print 2, import_target.__name__, os.path.normpath(import_target.__file__).replace(".pyc", ".py")
