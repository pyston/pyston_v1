import sys

sys.stdout.write("hello world\n")
print >>sys.stdout, "hello world"

print sys.stdout.fileno()
