# we have one test at global scope, another at local.
# they behave differently in codegen; there have been points at which either was bugged when the other was not.
try:
    class C(object):
        print 'here'
finally:
    print 'finally'

def f():
    try:
        class C(object):
            print 'here'
    finally:
        print 'finally'
f()

try:
    class D(object):
        print 'here'
except:
    print 'impossible'
    raise

def f2():
    try:
        class D(object):
            print 'here'
    except:
        print 'impossible'
        print D
        raise
f2()
