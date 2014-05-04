# expected: fail
# - descriptors not implemented yet

def f1():
    class D(object):
        def __get__(self, instance, owner):
            print "get", instance, owner
            return 1

        def __set__(self, instance, value):
            print "set", instance, value

    class C(object):
        d = D()

    print C.d
    print C().d

    c = C()
    c.d = 2
    print c.d
f1()

def f2():
    class MaybeDescriptorMeta(type):
        def __getattribute__(self, attr):
            print "meta __getattribute__", attr
            return 2

    class MaybeDescriptor(object):
        __metaclass__ = MaybeDescriptorMeta
        def __getattribute__(self, attr):
            print "__getattribute__", attr
            return 1

    class HasDescriptor(object):
        x = MaybeDescriptor()

    hd = HasDescriptor()
    # Getting hd.x will look up type(hd.__dict__[x]).__get__
    # and not go through __getattribute__
    print hd.x
    print hd.x.__get__
    print type(hd.x).__get__

    # But we can still set it here:
    def get(*args):
        print "get", args
        return 3
    hd.x.__get__ = get
    print hd.x
    type(hd.x).__get__ = get
    print hd.x
f2()
