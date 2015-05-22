# two-argument `raise' statements where second argument is itself an exception
class A(Exception): pass
class B(Exception): pass

def f():
    try: raise A, B(2)
    except A as e:
        print 'A', e
    except B as e:
        print 'B', e
f()
