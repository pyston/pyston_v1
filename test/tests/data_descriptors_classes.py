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
    # see what happens when we just use the class of the descriptor rather than an instance
    dd = DataDescriptor
    ndd = NonDataDescriptor

inst = C()
print 'ndd is %s' % str(inst.ndd)
inst.dd = 14
print 'dd is %s' % str(inst.dd)
inst.ndd = 20
print inst.ndd # should print out 20, having overridden the NonDataDescriptor

print 'C.dd is %s' % str(C.dd)
print 'C.ndd is %s', str(C.ndd)
C.dd = 6
C.ndd = 7
#TODO uncomment these:
#print C.__dict__['dd']
#print C.__dict__['ndd']
