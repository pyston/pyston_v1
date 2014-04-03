def f():
    global x # executed, simple case
    if 0:
        global z # never executed, doesn't matter
    x = 2
    y = 2
    z = 2
    w = 2
    print x, y, z, w

    # This generates a syntax warning:
    global w # executed after, still counts

x = 1
y = 1
z = 1
w = 1
f()
print x, y, z, w
