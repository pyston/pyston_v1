import sys
import os.path

print sys.version[:3]
print os.path.exists(sys.executable)
print sys.copyright[-200:]
print sys.byteorder
print sys.getdefaultencoding()
print sys.getfilesystemencoding()
