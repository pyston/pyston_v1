import sys
class TestClass(str):
    pass
t = TestClass("test_package")
sys.path.append(t)
import import_target
