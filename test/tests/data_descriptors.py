class DataDescriptor(object):
    def __get__(self, obj, type):
        print "__get__ called"
        if obj != None:
            return obj.a_store
    def __set__(self, obj, value):
        print "__set__ called with value", value
        obj.a_store = value + 1

class NonDataDescriptor(object):
    def __get__(self, obj, type):
        return 2
    # Don't define __set__

class C(object):
    dd = DataDescriptor()
    ndd = NonDataDescriptor()

inst = C()
print 'ndd is %s' % str(inst.ndd)
inst.dd = 14
print 'dd is %s' % str(inst.dd)
inst.ndd = 20
print inst.ndd # should print out 20, having overridden the NonDataDescriptor

inst.dd2 = 99
C.dd2 = DataDescriptor()
print 'inst.dd2 is %s' % str(inst.dd2)

print 'C.dd is %s' % str(C.dd)
print 'C.ndd is %s' % str(C.ndd)
C.dd = 6
C.ndd = 7
print C.__dict__['dd']
print C.__dict__['ndd']
print C.dd
print C.ndd

# Repeat all of the above for subclasses of the descriptors

class SubDataDescriptor(DataDescriptor):
    pass
class SubNonDataDescriptor(NonDataDescriptor):
    pass
class D(object):
    dd = SubDataDescriptor()
    ndd = SubNonDataDescriptor()

inst = D()
print 'ndd is %d' % inst.ndd
inst.dd = 14
print 'dd is %d' % inst.dd
inst.ndd = 20
print inst.ndd # should print out 20, having overridden the NonDataDescriptor

inst.dd2 = 99
C.dd2 = DataDescriptor()
print 'inst.dd2 is %s' % str(inst.dd2)

print 'D.dd is %s' % str(D.dd)
print 'D.ndd is %s' % str(D.ndd)
D.dd = 6
D.ndd = 7
print D.__dict__['dd']
print D.__dict__['ndd']

class DataDescriptor(object):
    def __get__(self, obj, type): return 1
    def __set__(self, obj, value): pass

class NonDataDescriptor(object):
    def __get__(self, obj, type): return 2
    # Don't define __set__

class C1(object):
    a = DataDescriptor()
class D1(C1):
    a = 3
d1 = D1()
print d1.a

print 'D1.a is %s' % str(D1.a)
D1.a = 6
print D1.__dict__['a']

class C2(object):
    a = 4
class D2(C2):
    a = DataDescriptor()
d2 = D2()
print d2.a

print 'D2.a is %s' % str(D2.a)
D2.a = 6
print D2.__dict__['a']

class C3(object):
    a = NonDataDescriptor()
class D3(C3):
    a = 5
d3 = D3()
print d3.a

print 'D3.a is %s' % str(D3.a)
D3.a = 6
print D3.__dict__['a']

class C4(object):
    a = 6
class D4(C4):
    a = NonDataDescriptor()
d4 = D4()
print d4.a

print 'D4.a is %s' % str(D4.a)
D4.a = 6
print D4.__dict__['a']
