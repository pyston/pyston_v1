import sys

f = open("/dev/null")
print repr(f.read())

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

f = open("/dev/null", 'w')
print chop(str(f))
f.close()
print chop(str(f))

f = open("/dev/null", 'r')
print chop(str(f))
f.close()
print chop(str(f))
