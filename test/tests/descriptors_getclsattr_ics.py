# run_args: -n
# statcheck: stats['slowpath_getclsattr'] <= 60

# Note: difference between DataDescriptor and NonDataDescriptor shouldn't matter
# for getclsattr (which is how __exit__ is looked up for with statements)
class DataDescriptor(object):
    def __get__(self, obj, typ):
        print '__get__ called'
        print type(self)
        print type(obj)
        print typ
        def exit(type, value, traceback):
            print 'exit called (1)'
        return exit
    def __set__(self, obj, value):
        pass

class NonDataDescriptor(object):
    def __get__(self, obj, typ):
        print '__get__ called'
        print type(self)
        print type(obj)
        print typ
        def exit(type, value, traceback):
            print 'exit called (2)'
        return exit

class C1(object):
    __exit__ = DataDescriptor()
    def __enter__(self):
        pass

class C2(object):
    __exit__ = NonDataDescriptor()
    def __enter__(self):
        pass

class C3(object):
    __exit__ = NonDataDescriptor()
    def __enter__(self):
        pass

class C4(object):
    def __enter__(self):
        pass

c1 = C1()
c2 = C2()
c3 = C3()
c4 = C4()

def exit3(type, value, traceback):
    print 'exit called (3)'
c3.__exit__ = exit3 # this should not get called

def exit4(type, value, traceback):
    print 'exit called (4)'
c4.__exit__ = exit4 # this should not get called

C4.__exit__ = DataDescriptor()

def f():
    with c1:
        print 'in with statement (1)'

    with c2:
        print 'in with statement (2)'

    with c3:
        print 'in with statement (3)'

    with c4:
        print 'in with statement (4)'

for i in xrange(1000):
    f()
