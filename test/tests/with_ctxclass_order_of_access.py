class Mgr(object):
    def __init__(self): print 'Mgr.__init__()'

    @property
    def __enter__(self):
        print 'Mgr.__enter__ accessed'
        def enterer(*args):
            print 'Mgr.__enter__ called'
            return 23
        return enterer

    @property
    def __exit__(self):
        print 'Mgr.__exit__ accessed'
        def exiter(*args):
            print 'Mgr.__exit__ called'
            return False
        return exiter

with Mgr() as m:
    print 'hello I am a with block'
    print 'm: %s' % m

try:
    with Mgr() as m:
        1/0
    print "you can't get there from here"
except ZeroDivisionError, e:
    print e
