# TODO This is a hodgepodge of stuff, should probably organize it better
# maybe merge some of it into dunder_descriptors?

class Descriptor2(object):
    def __get__(self, obj, type):
        def get2(self, obj, type):
            print '__get__ called'
            print self
            print obj
            print type
        return get2

class Descriptor(object):
    __get__ = Descriptor2()

class DescriptorNonzero(object):
    def __get__(self, obj, type):
        print 'nonzero __get__ called'
        def nonzero():
            print 'nonzero called'
            return True
        return nonzero

class C(object):
    desc = Descriptor()
    __nonzero__ = DescriptorNonzero()

# Should throw type error: Descriptor2 is not callable
try:
    print C().desc
except TypeError:
    # TODO should print the whole stacktrace and error message probably
    print 'got type error (1)'

# Should print True; in particular, it should look up __nonzero__
# using the descriptor protocol
if C():
    print 'True'
else:
    print 'False'
# this should *not* override it
c = C()
c.__nonzero__ = lambda x : False
if c:
    print 'True'
else:
    print 'False'
# this should
C.__nonzero__ = lambda x : False
if c:
    print 'True'
else:
    print 'False'

# __getattr__ and __setattr__
class DescriptorGetattr(object):
    def __get__(self, obj, type):
        print 'getattr __get__ called'
        def getattr(attr):
            print 'getattr called for attr', attr
            return 1337
        return getattr
class DescriptorSetattr(object):
    def __get__(self, obj, type):
        print 'setattr __get__ called'
        def setattr(attr, val):
            print 'setattr called for attr', attr, val
class D(object):
    __getattr__ = DescriptorGetattr()
    __setattr__ = DescriptorSetattr()

d = D()
try:
    print d.a
except TypeError:
    print 'got type error (2)'

#TODO enable this once __setattr__ is implemented
#try:
#    d.b = 12
#except TypeError:
#    print 'got type error (3)'

# with, __enter__ and __exit__
class DescriptorEnter(object):
    def __get__(self, obj, type):
        print 'enter __get__ called'
        def enter():
            print 'enter called'
            return 1337
        return enter
class DescriptorExit(object):
    def __get__(self, obj, type):
        print 'exit __get__ called'
        def exit(type, value, traceback):
            print 'enter called'
            return 1337
        return exit
class E(object):
    __enter__ = DescriptorEnter()
    __exit__ = DescriptorExit()
with E():
    print 'in with'

# what if __get__ is just an instance attribute
class Descriptor(object):
    pass
desc = Descriptor()
desc.__get__ = lambda self, obj, type : 5
def s(self, obj, value):
    print 'in __set__'
desc.__set__ = s
class F(object):
    at = desc
f = F()
print type(f.at) # should not call __get__
f.at = 12 # should not call __set__, should print nothing
#TODO uncomment this:
#print f.__dict__['at']


# test if we support getset.__get__ with default args
print type(5).__dict__["real"].__get__(42)
