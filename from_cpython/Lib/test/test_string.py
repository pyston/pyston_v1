# skip-if: True
# expected: fail
# this test gets stuck in an infinite loop(?) in test_replace (from string_tests)
import unittest, string
from test import test_support, string_tests
from UserList import UserList

class StringTest(
    string_tests.CommonTest,
    string_tests.MixinStrStringUserStringTest
    ):

    type2test = str

    def checkequal(self, result, object, methodname, *args):
        realresult = getattr(string, methodname)(object, *args)
        self.assertEqual(
            result,
            realresult
        )

    def checkraises(self, exc, obj, methodname, *args):
        with self.assertRaises(exc) as cm:
            getattr(string, methodname)(obj, *args)
        self.assertNotEqual(cm.exception.args[0], '')

    def checkcall(self, object, methodname, *args):
        getattr(string, methodname)(object, *args)

    def test_join(self):
        # These are the same checks as in string_test.ObjectTest.test_join
        # but the argument order ist different
        self.checkequal('a b c d', ['a', 'b', 'c', 'd'], 'join', ' ')
        self.checkequal('abcd', ('a', 'b', 'c', 'd'), 'join', '')
        self.checkequal('w x y z', string_tests.Sequence(), 'join', ' ')
        self.checkequal('abc', ('abc',), 'join', 'a')
        self.checkequal('z', UserList(['z']), 'join', 'a')
        if test_support.have_unicode:
            self.checkequal(unicode('a.b.c'), ['a', 'b', 'c'], 'join', unicode('.'))
            self.checkequal(unicode('a.b.c'), [unicode('a'), 'b', 'c'], 'join', '.')
            self.checkequal(unicode('a.b.c'), ['a', unicode('b'), 'c'], 'join', '.')
            self.checkequal(unicode('a.b.c'), ['a', 'b', unicode('c')], 'join', '.')
            self.checkraises(TypeError, ['a', unicode('b'), 3], 'join', '.')
        for i in [5, 25, 125]:
            self.checkequal(
                ((('a' * i) + '-') * i)[:-1],
                ['a' * i] * i, 'join', '-')
            self.checkequal(
                ((('a' * i) + '-') * i)[:-1],
                ('a' * i,) * i, 'join', '-')

        self.checkraises(TypeError, string_tests.BadSeq1(), 'join', ' ')
        self.checkequal('a b c', string_tests.BadSeq2(), 'join', ' ')
        try:
            def f():
                yield 4 + ""
            self.fixtype(' ').join(f())
        except TypeError, e:
            if '+' not in str(e):
                self.fail('join() ate exception message')
        else:
            self.fail('exception not raised')




class ModuleTest(unittest.TestCase):

    def test_attrs(self):
        string.whitespace
        string.lowercase
        string.uppercase
        string.letters
        string.digits
        string.hexdigits
        string.octdigits
        string.punctuation
        string.printable

    def test_atoi(self):
        self.assertEqual(string.atoi(" 1 "), 1)
        self.assertRaises(ValueError, string.atoi, " 1x")
        self.assertRaises(ValueError, string.atoi, " x1 ")

    def test_atol(self):
        self.assertEqual(string.atol("  1  "), 1L)
        self.assertRaises(ValueError, string.atol, "  1x ")
        self.assertRaises(ValueError, string.atol, "  x1 ")

    def test_atof(self):
        self.assertAlmostEqual(string.atof("  1  "), 1.0)
        self.assertRaises(ValueError, string.atof, "  1x ")
        self.assertRaises(ValueError, string.atof, "  x1 ")

    def test_maketrans(self):
        transtable = '\000\001\002\003\004\005\006\007\010\011\012\013\014\015\016\017\020\021\022\023\024\025\026\027\030\031\032\033\034\035\036\037 !"#$%&\'()*+,-./0123456789:;<=>?@ABCDEFGHIJKLMNOPQRSTUVWXYZ[\\]^_`xyzdefghijklmnopqrstuvwxyz{|}~\177\200\201\202\203\204\205\206\207\210\211\212\213\214\215\216\217\220\221\222\223\224\225\226\227\230\231\232\233\234\235\236\237\240\241\242\243\244\245\246\247\250\251\252\253\254\255\256\257\260\261\262\263\264\265\266\267\270\271\272\273\274\275\276\277\300\301\302\303\304\305\306\307\310\311\312\313\314\315\316\317\320\321\322\323\324\325\326\327\330\331\332\333\334\335\336\337\340\341\342\343\344\345\346\347\350\351\352\353\354\355\356\357\360\361\362\363\364\365\366\367\370\371\372\373\374\375\376\377'

        self.assertEqual(string.maketrans('abc', 'xyz'), transtable)
        self.assertRaises(ValueError, string.maketrans, 'abc', 'xyzq')

    def test_capwords(self):
        self.assertEqual(string.capwords('abc def ghi'), 'Abc Def Ghi')
        self.assertEqual(string.capwords('abc\tdef\nghi'), 'Abc Def Ghi')
        self.assertEqual(string.capwords('abc\t   def  \nghi'), 'Abc Def Ghi')
        self.assertEqual(string.capwords('ABC DEF GHI'), 'Abc Def Ghi')
        self.assertEqual(string.capwords('ABC-DEF-GHI', '-'), 'Abc-Def-Ghi')
        self.assertEqual(string.capwords('ABC-def DEF-ghi GHI'), 'Abc-def Def-ghi Ghi')
        self.assertEqual(string.capwords('   aBc  DeF   '), 'Abc Def')
        self.assertEqual(string.capwords('\taBc\tDeF\t'), 'Abc Def')
        self.assertEqual(string.capwords('\taBc\tDeF\t', '\t'), '\tAbc\tDef\t')

    def test_formatter(self):
        fmt = string.Formatter()
        self.assertEqual(fmt.format("foo"), "foo")

        self.assertEqual(fmt.format("foo{0}", "bar"), "foobar")
        self.assertEqual(fmt.format("foo{1}{0}-{1}", "bar", 6), "foo6bar-6")
        self.assertEqual(fmt.format("-{arg!r}-", arg='test'), "-'test'-")

        # override get_value ############################################
        class NamespaceFormatter(string.Formatter):
            def __init__(self, namespace={}):
                string.Formatter.__init__(self)
                self.namespace = namespace

            def get_value(self, key, args, kwds):
                if isinstance(key, str):
                    try:
                        # Check explicitly passed arguments first
                        return kwds[key]
                    except KeyError:
                        return self.namespace[key]
                else:
                    string.Formatter.get_value(key, args, kwds)

        fmt = NamespaceFormatter({'greeting':'hello'})
        self.assertEqual(fmt.format("{greeting}, world!"), 'hello, world!')


        # override format_field #########################################
        class CallFormatter(string.Formatter):
            def format_field(self, value, format_spec):
                return format(value(), format_spec)

        fmt = CallFormatter()
        self.assertEqual(fmt.format('*{0}*', lambda : 'result'), '*result*')


        # override convert_field ########################################
        class XFormatter(string.Formatter):
            def convert_field(self, value, conversion):
                if conversion == 'x':
                    return None
                return super(XFormatter, self).convert_field(value, conversion)

        fmt = XFormatter()
        self.assertEqual(fmt.format("{0!r}:{0!x}", 'foo', 'foo'), "'foo':None")


        # override parse ################################################
        class BarFormatter(string.Formatter):
            # returns an iterable that contains tuples of the form:
            # (literal_text, field_name, format_spec, conversion)
            def parse(self, format_string):
                for field in format_string.split('|'):
                    if field[0] == '+':
                        # it's markup
                        field_name, _, format_spec = field[1:].partition(':')
                        yield '', field_name, format_spec, None
                    else:
                        yield field, None, None, None

        fmt = BarFormatter()
        self.assertEqual(fmt.format('*|+0:^10s|*', 'foo'), '*   foo    *')

        # test all parameters used
        class CheckAllUsedFormatter(string.Formatter):
            def check_unused_args(self, used_args, args, kwargs):
                # Track which arguments actually got used
                unused_args = set(kwargs.keys())
                unused_args.update(range(0, len(args)))

                for arg in used_args:
                    unused_args.remove(arg)

                if unused_args:
                    raise ValueError("unused arguments")

        fmt = CheckAllUsedFormatter()
        self.assertEqual(fmt.format("{0}", 10), "10")
        self.assertEqual(fmt.format("{0}{i}", 10, i=100), "10100")
        self.assertEqual(fmt.format("{0}{i}{1}", 10, 20, i=100), "1010020")
        self.assertRaises(ValueError, fmt.format, "{0}{i}{1}", 10, 20, i=100, j=0)
        self.assertRaises(ValueError, fmt.format, "{0}", 10, 20)
        self.assertRaises(ValueError, fmt.format, "{0}", 10, 20, i=100)
        self.assertRaises(ValueError, fmt.format, "{i}", 10, 20, i=100)

        # Alternate formatting is not supported
        self.assertRaises(ValueError, format, '', '#')
        self.assertRaises(ValueError, format, '', '#20')

class BytesAliasTest(unittest.TestCase):

    def test_builtin(self):
        self.assertTrue(str is bytes)

    def test_syntax(self):
        self.assertEqual(b"spam", "spam")
        self.assertEqual(br"egg\foo", "egg\\foo")
        self.assertTrue(type(b""), str)
        self.assertTrue(type(br""), str)

def test_main():
    test_support.run_unittest(StringTest, ModuleTest, BytesAliasTest)

if __name__ == "__main__":
    test_main()
