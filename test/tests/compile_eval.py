print compile
c = compile("a", "test.py", "eval")
print type(c), c.co_filename, c.co_name

print
a = 0
print eval(c)

print
a = 0
g = {'a':1}
print eval(c, g)

print
a = 0
g = {'a':1}
l = {'a':2}
print eval(c, g, l)

print
g = {}
exec """
c = compile("a", "test.py", "eval")
""" in g
a = 0
print eval(g['c'])

print
a = 0
g = {'a':1, '_c':c}
exec "print eval(_c)" in g
