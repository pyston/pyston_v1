x = 1
def f():
    if 0:
        # the del marks 'x' as a name written to in this scope
        del x
    print x
try:
    f()
except NameError, e:
    print e
