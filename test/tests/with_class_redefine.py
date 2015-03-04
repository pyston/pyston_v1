# fail-if: (('-O' in EXTRA_JIT_ARGS) or ('-n' in EXTRA_JIT_ARGS)) and 'release' not in IMAGE
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
