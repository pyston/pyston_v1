c = compile("a = 1; print a", "test.py", "exec")
print type(c), c.co_filename, c.co_name

print
a = 0
exec c
print a

print
a = 0
g = {}
exec c in g
print a, sorted(g.keys())

print
g = {}
exec """
c = compile("a = 1; print a", "test.py", "exec")
""" in g
a = 0
exec g['c']
print a, sorted(g.keys())

print
a = 0
g = {'_c':c}
exec "exec _c" in g
print a, sorted(g.keys())

print
c = compile("a = 1; b = 2; a - b; b - a; c = a - b; c; print c", "test.py", "single")
exec c

exec compile("'hello world'", "test.py", "single")

import sys
def new_displayhook(arg):
    print "my custom displayhook!"
    print arg
sys.displayhook = new_displayhook
exec compile("'hello world'", "test.py", "single")
