# Random regression test from implementing refcounting:

def f():
    f.x = f
f()
f()
f()
f()
f()
f()
f()
