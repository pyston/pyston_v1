from abc import ABCMeta

class SubClassHook:
    __metaclass__ = ABCMeta
    @classmethod
    def __subclasshook__(cls, C):
        return "test" in C.__dict__

class C1(object):
    pass

class C2(object):
    def test():
        return 1

for i in (C1, C2, int, SubClassHook):
    print i, issubclass(i, SubClassHook), isinstance(i(), SubClassHook)
