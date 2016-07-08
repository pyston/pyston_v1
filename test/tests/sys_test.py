import sys
import os.path

assert callable(sys.excepthook)
print sys.displayhook
print sys.version[:3]
print os.path.exists(sys.executable)
print sys.copyright[-200:]
print sys.byteorder
print sys.getdefaultencoding()
print type(sys.getfilesystemencoding())
print type(sys.maxsize)
print sys.stdout is sys.__stdout__
print sys.stderr is sys.__stderr__
print sys.stdin is sys.__stdin__
print type(sys.subversion)
print len(sys.subversion)

try:
    1/0
except ZeroDivisionError:
    # Our tester won't check the output of this, but at least we can make sure it exists and runs:
    sys.excepthook(*sys.exc_info())
