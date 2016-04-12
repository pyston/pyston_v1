# this tests are from cpythons test_compile.py
import unittest
from test import test_support

class TestSpecifics(unittest.TestCase):
    def test_exec_functional_style(self):
        # Exec'ing a tuple of length 2 works.
        g = {'b': 2}
        exec("a = b + 1", g)
        self.assertEqual(g['a'], 3)

        # As does exec'ing a tuple of length 3.
        l = {'b': 3}
        g = {'b': 5, 'c': 7}
        exec("a = b + c", g, l)
        self.assertNotIn('a', g)
        self.assertEqual(l['a'], 10)

        # Tuples not of length 2 or 3 are invalid.
        with self.assertRaises(TypeError):
            exec("a = b + 1",)

        with self.assertRaises(TypeError):
            exec("a = b + 1", {}, {}, {})

        # Can't mix and match the two calling forms.
        g = {'a': 3, 'b': 4}
        l = {}
        with self.assertRaises(TypeError):
            exec("a = b + 1", g) in g
        with self.assertRaises(TypeError):
            exec("a = b + 1", g, l) in g, l

    def test_exec_with_general_mapping_for_locals(self):

        class M:
            "Test mapping interface versus possible calls from eval()."
            def __getitem__(self, key):
                if key == 'a':
                    return 12
                raise KeyError
            def __setitem__(self, key, value):
                self.results = (key, value)
            def keys(self):
                return list('xyz')

        m = M()
        g = globals()
        exec 'z = a' in g, m
        self.assertEqual(m.results, ('z', 12))
        try:
            exec 'z = b' in g, m
        except NameError:
            pass
        else:
            self.fail('Did not detect a KeyError')
        exec 'z = dir()' in g, m
        self.assertEqual(m.results, ('z', list('xyz')))
        exec 'z = globals()' in g, m
        self.assertEqual(m.results, ('z', g))
        exec 'z = locals()' in g, m
        self.assertEqual(m.results, ('z', m))
        try:
            exec 'z = b' in m
        except TypeError:
            pass
        else:
            self.fail('Did not validate globals as a real dict')

        class A:
            "Non-mapping"
            pass
        m = A()
        try:
            exec 'z = a' in g, m
        except TypeError:
            pass
        else:
            self.fail('Did not validate locals as a mapping')

        # Verify that dict subclasses work as well
        class D(dict):
            def __getitem__(self, key):
                if key == 'a':
                    return 12
                return dict.__getitem__(self, key)
        d = D()
        exec 'z = a' in g, d
        self.assertEqual(d['z'], 12)

    def test_unicode_encoding(self):
        code = u"# -*- coding: utf-8 -*-\npass\n"
        self.assertRaises(SyntaxError, compile, code, "tmp", "exec")
def test_main():
    test_support.run_unittest(TestSpecifics)

if __name__ == "__main__":
    # pyston change: remove duration in test output
    # test_main()
    import sys, StringIO, re
    orig_stdout = sys.stdout
    out = StringIO.StringIO()
    sys.stdout = out
    test_main()
    sys.stdout = orig_stdout
    print re.sub(" [.0-9]+s", " TIME", out.getvalue())

