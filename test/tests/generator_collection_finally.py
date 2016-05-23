# We currently don't call finalizers when destroying a generator.
def G():
    try:
        yield 0
        yield 1
        print "end"
    except Exception as e:
        print e
    finally:
        print "finally"

def foo():
    g = G()
    print g.next()
    print g.next()
foo()
