import sys
import os.path

print sys.version[:3]
print os.path.exists(sys.executable)
print sys.copyright[-200:]
print sys.byteorder
print sys.getdefaultencoding()
print sys.getfilesystemencoding()
print type(sys.maxsize)
print sys.stdout is sys.__stdout__
print sys.stderr is sys.__stderr__
print sys.stdin is sys.__stdin__
