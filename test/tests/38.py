# setattr/getattr test: turns out functions are the only mutable objects I support right now

def f():
    pass
def g():
    pass

def print_x(o):
    print o.x

f.x = 1
g.x = 2
print_x(f)
print_x(g)
