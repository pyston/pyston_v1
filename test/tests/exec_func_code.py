# expected: fail
# - not currently supported

def f():
    global b
    b = 1
    print b

print
b = 0
g = {}
exec f.func_code in g
print b, sorted(g.keys())

print
b = 0
g = {'f':f}
exec "f()" in g
print b, sorted(g.keys())
