# allow-warning: converting unicode literal to str

import sys
import os.path

print sys.version[:3]
print os.path.exists(sys.executable)
print sys.prefix, sys.exec_prefix
