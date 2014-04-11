t = (1, "h")
print t, str(t), repr(t)
if 1:
    t = (3,)
print t

def f():
    t = (1, 3)
    print t
f()

print ()
print (1,)
print (1, 2)
print (1, 2, 3)

t = 1, 3
print t

print (2,) < (2,)
print (2,) < (2, 3)
print (3,) < (2, 3)

print
