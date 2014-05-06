# expected: fail
# - exceptions

def throw(x):
    try:
        raise x
    except Exception, e:
        print type(e)

# Both print "Exception"
throw(Exception)
throw(Exception())

def catches(t, e):
    try:
        raise e
    except t:
        return True
    except:
        return False

print catches(Exception, Exception)
print catches(Exception, Exception())
print catches(Exception(), Exception())
print catches(Exception, StopIteration())
print catches(Exception, RuntimeError())
print catches(Exception, KeyboardInterrupt())
print catches(None, Exception())
print catches(1, Exception())

_StopIteration = StopIteration
def f1():
    import __builtin__
    __builtin__.StopIteration = StopIteration = 0

    # This should still work:
    for i in xrange(5):
        print i

    __builtin__.StopIteration = _StopIteration
f1()

def f2(throw):
    print "f2"
    try:
        try:
            print "inner try"
            if throw:
                raise Exception()
        except:
            print "inner except"
        else:
            print "inner else"
            raise Exception()
    except:
        print "outer except"
    else:
        print "outer else"
f2(True)
f2(False)

