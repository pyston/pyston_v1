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
