import weakref

def test_wr(o):
    try:
        r = weakref.ref(o)
        print "passed", type(o)
        return r
    except:
        print "failed", type(o)

def test():
  1

wr = test_wr(test)
print wr() == test
#print weakref.getweakrefs(test)[0] == wr
print weakref.getweakrefcount(test)
