#

# currently broken:
# import os.path

import os

r1 = os.urandom(8)
r2 = os.urandom(8)
print len(r1), len(r2), type(r1), type(r2), r1 == r2

print type(os.stat("/dev/null"))

print os.path.expanduser("~") == os.environ["HOME"]

print os.path.isfile("/dev/null")
print os.path.isfile("/should_not_exist!")

e = OSError(1, 2, 3)
print e
print e.errno
print e.strerror
print e.filename
print OSError(1, 2).filename

# This part needs sys.exc_info() and the three-arg raise statement
# try:
    # os.execvp("aoeuaoeu", ['aoeuaoeu'])
# except OSError, e:
    # print e
