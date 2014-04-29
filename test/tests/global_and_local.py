# expected: fail
# - this particular check isn't implemented yet

# I would have expected this to be valid, but cPython and pypy err out saying "name 'x' is local and global"

print "first"

x = 1
def f(x):
    global x

print "calling"
f(2)
print x
