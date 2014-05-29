import sys


__package__ = "Test"
__builtins__ = None
__doc__ = None


def fake():
    # manually sorted: ['all', 'these', 'attributes', 'do', 'not', 'exist']
    return ['all', 'attributes', 'do', 'exist', 'not', 'these']


class TestClass(object):
    def __init__(self):
        self.__dict__ = {n: n for n in fake()}

    def __dir__(self):
        return fake()


class TestClass2(object):
    def __init__(self):
        self.attribute1 = None
        self.__dict__ = {'dictAttr': True}

    def method1(self):
        pass

# All attributes are different between CPython 2.7.5 and pyston
# CPython has more
dir(sys.modules[__name__])
dir(TestClass())
dir(TestClass2())
dir(str())
dir(list())
dir({})
dir(int())
# dir(long())
dir(dir)
dir(fake)
dir(None)
dir()

dir(TestClass)
dir(TestClass2)
dir(sys)
dir(dict)
dir(int)
dir(str)
dir(set)

tc = TestClass()
for f in dir(tc):
    print tc.__dict__[f]
