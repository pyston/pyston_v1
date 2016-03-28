import sys
import tempfile

f = open("/dev/null")
print repr(f.read())
print repr(f.name)

f2 = file("/dev/null")
print repr(f2.read())

with open("/dev/null") as f3:
    print repr(f3.read())

# String representation of file objects.
def chop(s):
    """Chop off the last bit of a file object repr, which is the address
    of the raw file pointer.
    """
    return ' '.join(s.split()[:-1])

for desc in [sys.stderr, sys.stdout, sys.stdin]:
    print chop(str(desc))
    print desc.encoding

f = open("/dev/null", 'w')
print chop(str(f))
f.close()
print chop(str(f))

f = open("/dev/null", 'r')
print chop(str(f))
f.close()
print chop(str(f))

# Support for iteration protocol.
f = open('/dev/null')
print iter(f) is f
f.close()

with open(__file__) as f:
    lines = list(f)
    print lines[:5]
    print lines[-5:]

with open(__file__) as f:
    print len(f.readlines())

# Check that opening a non-existent file results in an IOError.
try:
   f = open('this-should-definitely-not-exist.txt')
except IOError as e:
   print str(e)

f = open("/dev/null", "w")
print f.tell()
print f.write("hello world")
f.softspace = 0
print f.tell()
print f.seek(0)
print f.write("H")
print f.tell()
print f.flush()
print f.close()

# tests for universal newlines
fd, fn = tempfile.mkstemp()

with open(fn, "wb") as f:
    f.write("hello world!\r")
    f.write("hello world!\r\n")
    f.write("hello world!\n")
    f.write("hello world!\r")

with open(fn) as f:
    print len(f.readlines())

with open(fn, "rU") as f:
    print len(f.readlines())

with open(fn, "r", 1) as f:
    print len(f.readlines())

fd, fn = tempfile.mkstemp()
try:
    with open(fn, "wU") as f:
        print "succeeded"
except Exception as e:
    print e

try:
    with open(fn, "aU") as f:
        print "succeeded"
except Exception as e:
    print e

with open(fn, "w") as f:
    f.write("123456");
    f.truncate(3)
with open(fn, "r") as f:
    print f.read();

print sys.stdout.closed
