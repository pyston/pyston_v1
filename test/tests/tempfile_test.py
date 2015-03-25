import os
import tempfile

fd, dirname = tempfile.mkstemp()
print type(fd), type(dirname)

print os.path.exists(dirname)
os.unlink(dirname)
print os.path.exists(dirname)
