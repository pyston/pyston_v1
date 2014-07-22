# expected: fail
# - deletes on names

x = 1
def f():
    if 0:
        # the del marks 'x' as a name written to in this scope
        del x
    print x
f()
