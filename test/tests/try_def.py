def f():
    try:
        def foo(): return 0
    except:
        print 'except'
    finally:
        print 'finally'
f()

def f2():
    try:
        def foo(): return 0
    except:
        print 'except'
f2()

def f3():
    try:
        def foo(): return 0
    finally:
        print 'finally'
f3()
