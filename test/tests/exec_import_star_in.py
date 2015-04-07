# expected: fail

g = {}
exec """from import_target import *""" in g
del g['__builtins__']
print sorted(g.keys())
