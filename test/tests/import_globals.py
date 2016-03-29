import test_package.import_target
import import_target
print

s = """
import import_target
print import_target.__file__.replace(".pyc", ".py")
"""

def test(g):
    print "Testing with globals:", "None" if g is None else ("globals" if g is globals() else sorted(g.items()))
    print __import__("import_target", g).__file__.replace(".pyc", ".py")
    exec s in g
    print "Resulting globals:", "None" if g is None else ("globals" if g is globals() else sorted(g.keys()))
    print

test(None)
test(globals())

test({"__package__":"test_package"})

test({"__name__":"test_package.foo"})
test({"__name__":"test_package"})
test({"__name__":"test_package", "__path__":"foo"})
