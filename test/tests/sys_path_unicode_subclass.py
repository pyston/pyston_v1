import sys
class TestClass(unicode):
    pass
t = TestClass("test_package")
sys.path.append(t)
import import_target
