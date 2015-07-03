# run_args: -n
# statcheck: stats['slowpath_callattr'] <= 100

# Note: difference between DataDescriptor and NonDataDescriptor shouldn't matter
#  __enter__ is looked up via callattr with class_only set to true
class DataDescriptor(object):
    def __get__(self, obj, typ):
        print '__get__ called'
        print type(self)
        print type(obj)
        print typ
        def enter():
            print 'enter called (1)'
        return enter
    def __set__(self, obj, value):
        pass

class NonDataDescriptor(object):
    def __get__(self, obj, typ):
        print '__get__ called'
        print type(self)
        print type(obj)
        print typ
        def enter():
            print 'enter called (2)'
        return enter

class C1(object):
    __enter__ = DataDescriptor()
    def __exit__(self, typ, value, traceback):
        pass

class C2(object):
    __enter__ = NonDataDescriptor()
    def __exit__(self, typ, value, traceback):
        pass

class C3(object):
    __enter__ = NonDataDescriptor()
    def __exit__(self, typ, value, traceback):
        pass

class C4(object):
    def __exit__(self, typ, value, traceback):
        pass

c1 = C1()
c2 = C2()
c3 = C3()
c4 = C4()

def enter3(type):
    print 'enter called (3)'
c3.__enter__ = enter3 # this should not get called

def enter4(type):
    print 'enter called (4)'
c4.__enter__ = enter4 # this should not get called

C4.__enter__ = DataDescriptor()

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
