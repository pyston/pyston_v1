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
        self.__dict__ = {'dictAttr': False, 'attribute1': None}
        self.other_attribute = False

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


def test_in_dir(names, obj):
    r = dir(obj)
    print [n in r for n in names]

test_in_dir(fake(), TestClass())
test_in_dir(['attribute1', 'method1'], TestClass2)
test_in_dir(['attribute1', 'method1', 'dictAttr'], TestClass2())
test_in_dir(['__str__', '__new__', '__repr__', '__dir__', '__init__',
             '__module__'] + fake(), TestClass)
test_in_dir(['__str__', '__new__', '__repr__', '__dir__', '__init__',
             '__module__', 'method1', 'dictAttr', 'attribute1'], TestClass2)
test_in_dir(['attribute1', 'dictAttr', '__init__', '__module__', 'method1',
             'other_attribute', '__dict__'], TestClass2())
test_in_dir(fake(), TestClass())
print len(fake()) == len(dir(TestClass()))

for t in [str, int, list, set, dict]:
    test_in_dir(['__str__', '__new__', '__repr__', '__dir__', '__module__'], t)
