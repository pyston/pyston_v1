class IntLike(object):
    def __radd__(self, rhs):
        return "hello world"

def f():
    i = 1
    print i

    # Augassigns can change the type of the variable:
    i += IntLike()
    print i
    try:
        i + 1
    except TypeError, e:
        print e
f()
