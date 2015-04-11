d = {}
exec "a = 5" in d
print d['a']

def f():
    d2 = {}
    exec "b = 6" in d2
    print d2['b']
f()
