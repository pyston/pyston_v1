def f():
    C = 23
    try:
        class C(object): pass
    except:
        print 'except: C = %s' % C
        raise
    finally:
        print 'finally: C = %s' % C
    print 'end: C = %s' % C

f()
