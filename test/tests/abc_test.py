from abc import ABCMeta, abstractmethod

class C(object):
    __metaclass__ = ABCMeta

    @abstractmethod
    def foo(self):
        pass

try:
    C()
    assert 0
except TypeError as e:
    print e

def c_init(self):
    pass
C.__init__ = c_init

try:
    C()
    assert 0
except TypeError as e:
    print e
