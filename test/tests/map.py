def f(*args):
    print "f(",
    for a in args:
        print a,
    print ")"
    
    s = -1
    try:
        s = sum(args)
    except:
        pass
    return s

print map(f, range(10))
print map(f, range(9), range(20, 31), range(30, 40), range(40, 50), range(60, 70))
print map(None, range(10))
print map(None, range(9), range(20, 31), range(30, 40), range(40, 50), range(60, 70))

try:
    print map(lambda x: x*x, range(3))
except Exception as e:
    print e
try:
    print map(lambda x, y: x*y, range(3), range(4))
except Exception as e:
    print e
try:
    print map(lambda x: 1/x, range(3))
except Exception as e:
    print e
try:
    print map(lambda x, y: x/y, range(3), range(4))
except Exception as e:
    print e
