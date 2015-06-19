import ConfigParser
import StringIO
import os
import unittest
import UserDict

from test import test_support


class SortedDict(UserDict.UserDict):
    def items(self):
        result = self.data.items()
        result.sort()
        return result

    def keys(self):
        result = self.data.keys()
        result.sort()
        return result

    def values(self):
        # XXX never used?
        result = self.items()
        return [i[1] for i in result]

    def iteritems(self): return iter(self.items())
    def iterkeys(self): return iter(self.keys())
    __iter__ = iterkeys
    def itervalues(self): return iter(self.values())


class TestCaseBase(unittest.TestCase):
    allow_no_value = False

    def newconfig(self, defaults=None):
        if defaults is None:
            self.cf = self.config_class(allow_no_value=self.allow_no_value)
        else:
            self.cf = self.config_class(defaults,
                                        allow_no_value=self.allow_no_value)
        return self.cf

    def fromstring(self, string, defaults=None):
        cf = self.newconfig(defaults)
        sio = StringIO.StringIO(string)
        cf.readfp(sio)
        return cf

    def test_basic(self):
        config_string = (
            "[Foo Bar]\n"
            "foo=bar\n"
            "[Spacey Bar]\n"
            "foo = bar\n"
            "[Commented Bar]\n"
            "foo: bar ; comment\n"
            "[Long Line]\n"
            "foo: this line is much, much longer than my editor\n"
            "   likes it.\n"
            "[Section\\with$weird%characters[\t]\n"
            "[Internationalized Stuff]\n"
            "foo[bg]: Bulgarian\n"
            "foo=Default\n"
            "foo[en]=English\n"
            "foo[de]=Deutsch\n"
            "[Spaces]\n"
            "key with spaces : value\n"
            "another with spaces = splat!\n"
            )
        if self.allow_no_value:
            config_string += (
                "[NoValue]\n"
                "option-without-value\n"
                )

        cf = self.fromstring(config_string)
        L = cf.sections()
        L.sort()
        E = [r'Commented Bar',
             r'Foo Bar',
             r'Internationalized Stuff',
             r'Long Line',
             r'Section\with$weird%characters[' '\t',
             r'Spaces',
             r'Spacey Bar',
             ]
        if self.allow_no_value:
            E.append(r'NoValue')
        E.sort()
        eq = self.assertEqual
        eq(L, E)

        # The use of spaces in the section names serves as a
        # regression test for SourceForge bug #583248:
        # http://www.python.org/sf/583248
        eq(cf.get('Foo Bar', 'foo'), 'bar')
        eq(cf.get('Spacey Bar', 'foo'), 'bar')
        eq(cf.get('Commented Bar', 'foo'), 'bar')
        eq(cf.get('Spaces', 'key with spaces'), 'value')
        eq(cf.get('Spaces', 'another with spaces'), 'splat!')
        if self.allow_no_value:
            eq(cf.get('NoValue', 'option-without-value'), None)

        self.assertNotIn('__name__', cf.options("Foo Bar"),
                         '__name__ "option" should not be exposed by the API!')

        # Make sure the right things happen for remove_option();
        # added to include check for SourceForge bug #123324:
        self.assertTrue(cf.remove_option('Foo Bar', 'foo'),
                        "remove_option() failed to report existence of option")
        self.assertFalse(cf.has_option('Foo Bar', 'foo'),
                    "remove_option() failed to remove option")
        self.assertFalse(cf.remove_option('Foo Bar', 'foo'),
                    "remove_option() failed to report non-existence of option"
                    " that was removed")

        self.assertRaises(ConfigParser.NoSectionError,
                          cf.remove_option, 'No Such Section', 'foo')

        eq(cf.get('Long Line', 'foo'),
           'this line is much, much longer than my editor\nlikes it.')

    def test_case_sensitivity(self):
        cf = self.newconfig()
        cf.add_section("A")
        cf.add_section("a")
        L = cf.sections()
        L.sort()
        eq = self.assertEqual
        eq(L, ["A", "a"])
        cf.set("a", "B", "value")
        eq(cf.options("a"), ["b"])
        eq(cf.get("a", "b"), "value",
           "could not locate option, expecting case-insensitive option names")
        self.assertTrue(cf.has_option("a", "b"))
        cf.set("A", "A-B", "A-B value")
        for opt in ("a-b", "A-b", "a-B", "A-B"):
            self.assertTrue(
                cf.has_option("A", opt),
                "has_option() returned false for option which should exist")
        eq(cf.options("A"), ["a-b"])
        eq(cf.options("a"), ["b"])
        cf.remove_option("a", "B")
        eq(cf.options("a"), [])

        # SF bug #432369:
        cf = self.fromstring(
            "[MySection]\nOption: first line\n\tsecond line\n")
        eq(cf.options("MySection"), ["option"])
        eq(cf.get("MySection", "Option"), "first line\nsecond line")

        # SF bug #561822:
        cf = self.fromstring("[section]\nnekey=nevalue\n",
                             defaults={"key":"value"})
        self.assertTrue(cf.has_option("section", "Key"))


    def test_default_case_sensitivity(self):
        cf = self.newconfig({"foo": "Bar"})
        self.assertEqual(
            cf.get("DEFAULT", "Foo"), "Bar",
            "could not locate option, expecting case-insensitive option names")
        cf = self.newconfig({"Foo": "Bar"})
        self.assertEqual(
            cf.get("DEFAULT", "Foo"), "Bar",
            "could not locate option, expecting case-insensitive defaults")

    def test_parse_errors(self):
        self.newconfig()
        self.parse_error(ConfigParser.ParsingError,
                         "[Foo]\n  extra-spaces: splat\n")
        self.parse_error(ConfigParser.ParsingError,
                         "[Foo]\n  extra-spaces= splat\n")
        self.parse_error(ConfigParser.ParsingError,
                         "[Foo]\n:value-without-option-name\n")
        self.parse_error(ConfigParser.ParsingError,
                         "[Foo]\n=value-without-option-name\n")
        self.parse_error(ConfigParser.MissingSectionHeaderError,
                         "No Section!\n")

    def parse_error(self, exc, src):
        sio = StringIO.StringIO(src)
        self.assertRaises(exc, self.cf.readfp, sio)

    def test_query_errors(self):
        cf = self.newconfig()
        self.assertEqual(cf.sections(), [],
                         "new ConfigParser should have no defined sections")
        self.assertFalse(cf.has_section("Foo"),
                         "new ConfigParser should have no acknowledged "
                         "sections")
        self.assertRaises(ConfigParser.NoSectionError,
                          cf.options, "Foo")
        self.assertRaises(ConfigParser.NoSectionError,
                          cf.set, "foo", "bar", "value")
        self.get_error(ConfigParser.NoSectionError, "foo", "bar")
        cf.add_section("foo")
        self.get_error(ConfigParser.NoOptionError, "foo", "bar")

    def get_error(self, exc, section, option):
        try:
            self.cf.get(section, option)
        except exc, e:
            return e
        else:
            self.fail("expected exception type %s.%s"
                      % (exc.__module__, exc.__name__))

    def test_boolean(self):
        cf = self.fromstring(
            "[BOOLTEST]\n"
            "T1=1\n"
            "T2=TRUE\n"
            "T3=True\n"
            "T4=oN\n"
            "T5=yes\n"
            "F1=0\n"
            "F2=FALSE\n"
            "F3=False\n"
            "F4=oFF\n"
            "F5=nO\n"
            "E1=2\n"
            "E2=foo\n"
            "E3=-1\n"
            "E4=0.1\n"
            "E5=FALSE AND MORE"
            )
        for x in range(1, 5):
            self.assertTrue(cf.getboolean('BOOLTEST', 't%d' % x))
            self.assertFalse(cf.getboolean('BOOLTEST', 'f%d' % x))
            self.assertRaises(ValueError,
                              cf.getboolean, 'BOOLTEST', 'e%d' % x)

    def test_weird_errors(self):
        cf = self.newconfig()
        cf.add_section("Foo")
        self.assertRaises(ConfigParser.DuplicateSectionError,
                          cf.add_section, "Foo")

    def test_write(self):
        config_string = (
            "[Long Line]\n"
            "foo: this line is much, much longer than my editor\n"
            "   likes it.\n"
            "[DEFAULT]\n"
            "foo: another very\n"
            " long line\n"
            )
        if self.allow_no_value:
            config_string += (
            "[Valueless]\n"
            "option-without-value\n"
            )

        cf = self.fromstring(config_string)
        output = StringIO.StringIO()
        cf.write(output)
        expect_string = (
            "[DEFAULT]\n"
            "foo = another very\n"
            "\tlong line\n"
            "\n"
            "[Long Line]\n"
            "foo = this line is much, much longer than my editor\n"
            "\tlikes it.\n"
            "\n"
            )
        if self.allow_no_value:
            expect_string += (
                "[Valueless]\n"
                "option-without-value\n"
                "\n"
                )
        self.assertEqual(output.getvalue(), expect_string)

    def test_set_string_types(self):
        cf = self.fromstring("[sect]\n"
                             "option1=foo\n")
        # Check that we don't get an exception when setting values in
        # an existing section using strings:
        class mystr(str):
            pass
        cf.set("sect", "option1", "splat")
        cf.set("sect", "option1", mystr("splat"))
        cf.set("sect", "option2", "splat")
        cf.set("sect", "option2", mystr("splat"))

    def test_set_unicode(self):
        try:
            unicode
        except NameError:
            self.skipTest('no unicode support')

        cf = self.fromstring("[sect]\n"
                             "option1=foo\n")
        cf.set("sect", "option1", unicode("splat"))
        cf.set("sect", "option2", unicode("splat"))

    def test_read_returns_file_list(self):
        file1 = test_support.findfile("cfgparser.1")
        # check when we pass a mix of readable and non-readable files:
        cf = self.newconfig()
        parsed_files = cf.read([file1, "nonexistent-file"])
        self.assertEqual(parsed_files, [file1])
        self.assertEqual(cf.get("Foo Bar", "foo"), "newbar")
        # check when we pass only a filename:
        cf = self.newconfig()
        parsed_files = cf.read(file1)
        self.assertEqual(parsed_files, [file1])
        self.assertEqual(cf.get("Foo Bar", "foo"), "newbar")
        # check when we pass only missing files:
        cf = self.newconfig()
        parsed_files = cf.read(["nonexistent-file"])
        self.assertEqual(parsed_files, [])
        # check when we pass no files:
        cf = self.newconfig()
        parsed_files = cf.read([])
        self.assertEqual(parsed_files, [])

    # shared by subclasses
    def get_interpolation_config(self):
        return self.fromstring(
            "[Foo]\n"
            "bar=something %(with1)s interpolation (1 step)\n"
            "bar9=something %(with9)s lots of interpolation (9 steps)\n"
            "bar10=something %(with10)s lots of interpolation (10 steps)\n"
            "bar11=something %(with11)s lots of interpolation (11 steps)\n"
            "with11=%(with10)s\n"
            "with10=%(with9)s\n"
            "with9=%(with8)s\n"
            "with8=%(With7)s\n"
            "with7=%(WITH6)s\n"
            "with6=%(with5)s\n"
            "With5=%(with4)s\n"
            "WITH4=%(with3)s\n"
            "with3=%(with2)s\n"
            "with2=%(with1)s\n"
            "with1=with\n"
            "\n"
            "[Mutual Recursion]\n"
            "foo=%(bar)s\n"
            "bar=%(foo)s\n"
            "\n"
            "[Interpolation Error]\n"
            "name=%(reference)s\n",
            # no definition for 'reference'
            defaults={"getname": "%(__name__)s"})

    def check_items_config(self, expected):
        cf = self.fromstring(
            "[section]\n"
            "name = value\n"
            "key: |%(name)s| \n"
            "getdefault: |%(default)s|\n"
            "getname: |%(__name__)s|",
            defaults={"default": "<default>"})
        L = list(cf.items("section"))
        L.sort()
        self.assertEqual(L, expected)


class ConfigParserTestCase(TestCaseBase):
    config_class = ConfigParser.ConfigParser
    allow_no_value = True

    def test_interpolation(self):
        rawval = {
            ConfigParser.ConfigParser: ("something %(with11)s "
                                        "lots of interpolation (11 steps)"),
            ConfigParser.SafeConfigParser: "%(with1)s",
        }
        cf = self.get_interpolation_config()
        eq = self.assertEqual
        eq(cf.get("Foo", "getname"), "Foo")
        eq(cf.get("Foo", "bar"), "something with interpolation (1 step)")
        eq(cf.get("Foo", "bar9"),
           "something with lots of interpolation (9 steps)")
        eq(cf.get("Foo", "bar10"),
           "something with lots of interpolation (10 steps)")
        self.get_error(ConfigParser.InterpolationDepthError, "Foo", "bar11")

    def test_interpolation_missing_value(self):
        self.get_interpolation_config()
        e = self.get_error(ConfigParser.InterpolationError,
                           "Interpolation Error", "name")
        self.assertEqual(e.reference, "reference")
        self.assertEqual(e.section, "Interpolation Error")
        self.assertEqual(e.option, "name")

    def test_items(self):
        self.check_items_config([('default', '<default>'),
                                 ('getdefault', '|<default>|'),
                                 ('getname', '|section|'),
                                 ('key', '|value|'),
                                 ('name', 'value')])

    def test_set_nonstring_types(self):
        cf = self.newconfig()
        cf.add_section('non-string')
        cf.set('non-string', 'int', 1)
        cf.set('non-string', 'list', [0, 1, 1, 2, 3, 5, 8, 13, '%('])
        cf.set('non-string', 'dict', {'pi': 3.14159, '%(': 1,
                                      '%(list)': '%(list)'})
        cf.set('non-string', 'string_with_interpolation', '%(list)s')
        cf.set('non-string', 'no-value')
        self.assertEqual(cf.get('non-string', 'int', raw=True), 1)
        self.assertRaises(TypeError, cf.get, 'non-string', 'int')
        self.assertEqual(cf.get('non-string', 'list', raw=True),
                         [0, 1, 1, 2, 3, 5, 8, 13, '%('])
        self.assertRaises(TypeError, cf.get, 'non-string', 'list')
        self.assertEqual(cf.get('non-string', 'dict', raw=True),
                         {'pi': 3.14159, '%(': 1, '%(list)': '%(list)'})
        self.assertRaises(TypeError, cf.get, 'non-string', 'dict')
        self.assertEqual(cf.get('non-string', 'string_with_interpolation',
                                raw=True), '%(list)s')
        self.assertRaises(ValueError, cf.get, 'non-string',
                          'string_with_interpolation', raw=False)
        self.assertEqual(cf.get('non-string', 'no-value'), None)

class MultilineValuesTestCase(TestCaseBase):
    config_class = ConfigParser.ConfigParser
    wonderful_spam = ("I'm having spam spam spam spam "
                      "spam spam spam beaked beans spam "
                      "spam spam and spam!").replace(' ', '\t\n')

    def setUp(self):
        cf = self.newconfig()
        for i in range(100):
            s = 'section{}'.format(i)
            cf.add_section(s)
            for j in range(10):
                cf.set(s, 'lovely_spam{}'.format(j), self.wonderful_spam)
        with open(test_support.TESTFN, 'w') as f:
            cf.write(f)

    def tearDown(self):
        os.unlink(test_support.TESTFN)

    def test_dominating_multiline_values(self):
        # we're reading from file because this is where the code changed
        # during performance updates in Python 3.2
        cf_from_file = self.newconfig()
        with open(test_support.TESTFN) as f:
            cf_from_file.readfp(f)
        self.assertEqual(cf_from_file.get('section8', 'lovely_spam4'),
                         self.wonderful_spam.replace('\t\n', '\n'))

class RawConfigParserTestCase(TestCaseBase):
    config_class = ConfigParser.RawConfigParser

    def test_interpolation(self):
        cf = self.get_interpolation_config()
        eq = self.assertEqual
        eq(cf.get("Foo", "getname"), "%(__name__)s")
        eq(cf.get("Foo", "bar"),
           "something %(with1)s interpolation (1 step)")
        eq(cf.get("Foo", "bar9"),
           "something %(with9)s lots of interpolation (9 steps)")
        eq(cf.get("Foo", "bar10"),
           "something %(with10)s lots of interpolation (10 steps)")
        eq(cf.get("Foo", "bar11"),
           "something %(with11)s lots of interpolation (11 steps)")

    def test_items(self):
        self.check_items_config([('default', '<default>'),
                                 ('getdefault', '|%(default)s|'),
                                 ('getname', '|%(__name__)s|'),
                                 ('key', '|%(name)s|'),
                                 ('name', 'value')])

    def test_set_nonstring_types(self):
        cf = self.newconfig()
        cf.add_section('non-string')
        cf.set('non-string', 'int', 1)
        cf.set('non-string', 'list', [0, 1, 1, 2, 3, 5, 8, 13])
        cf.set('non-string', 'dict', {'pi': 3.14159})
        self.assertEqual(cf.get('non-string', 'int'), 1)
        self.assertEqual(cf.get('non-string', 'list'),
                         [0, 1, 1, 2, 3, 5, 8, 13])
        self.assertEqual(cf.get('non-string', 'dict'), {'pi': 3.14159})


class SafeConfigParserTestCase(ConfigParserTestCase):
    config_class = ConfigParser.SafeConfigParser

    def test_safe_interpolation(self):
        # See http://www.python.org/sf/511737
        cf = self.fromstring("[section]\n"
                             "option1=xxx\n"
                             "option2=%(option1)s/xxx\n"
                             "ok=%(option1)s/%%s\n"
                             "not_ok=%(option2)s/%%s")
        self.assertEqual(cf.get("section", "ok"), "xxx/%s")
        self.assertEqual(cf.get("section", "not_ok"), "xxx/xxx/%s")

    def test_set_malformatted_interpolation(self):
        cf = self.fromstring("[sect]\n"
                             "option1=foo\n")

        self.assertEqual(cf.get('sect', "option1"), "foo")

        self.assertRaises(ValueError, cf.set, "sect", "option1", "%foo")
        self.assertRaises(ValueError, cf.set, "sect", "option1", "foo%")
        self.assertRaises(ValueError, cf.set, "sect", "option1", "f%oo")

        self.assertEqual(cf.get('sect', "option1"), "foo")

        # bug #5741: double percents are *not* malformed
        cf.set("sect", "option2", "foo%%bar")
        self.assertEqual(cf.get("sect", "option2"), "foo%bar")

    def test_set_nonstring_types(self):
        cf = self.fromstring("[sect]\n"
                             "option1=foo\n")
        # Check that we get a TypeError when setting non-string values
        # in an existing section:
        self.assertRaises(TypeError, cf.set, "sect", "option1", 1)
        self.assertRaises(TypeError, cf.set, "sect", "option1", 1.0)
        self.assertRaises(TypeError, cf.set, "sect", "option1", object())
        self.assertRaises(TypeError, cf.set, "sect", "option2", 1)
        self.assertRaises(TypeError, cf.set, "sect", "option2", 1.0)
        self.assertRaises(TypeError, cf.set, "sect", "option2", object())

    def test_add_section_default_1(self):
        cf = self.newconfig()
        self.assertRaises(ValueError, cf.add_section, "default")

    def test_add_section_default_2(self):
        cf = self.newconfig()
        self.assertRaises(ValueError, cf.add_section, "DEFAULT")


class SafeConfigParserTestCaseNoValue(SafeConfigParserTestCase):
    allow_no_value = True

class TestChainMap(unittest.TestCase):
    def test_issue_12717(self):
        d1 = dict(red=1, green=2)
        d2 = dict(green=3, blue=4)
        dcomb = d2.copy()
        dcomb.update(d1)
        cm = ConfigParser._Chainmap(d1, d2)
        self.assertIsInstance(cm.keys(), list)
        self.assertEqual(set(cm.keys()), set(dcomb.keys()))      # keys()
        self.assertEqual(set(cm.values()), set(dcomb.values()))  # values()
        self.assertEqual(set(cm.items()), set(dcomb.items()))    # items()
        self.assertEqual(set(cm), set(dcomb))                    # __iter__ ()
        self.assertEqual(cm, dcomb)                              # __eq__()
        self.assertEqual([cm[k] for k in dcomb], dcomb.values()) # __getitem__()
        klist = 'red green blue black brown'.split()
        self.assertEqual([cm.get(k, 10) for k in klist],
                         [dcomb.get(k, 10) for k in klist])      # get()
        self.assertEqual([k in cm for k in klist],
                         [k in dcomb for k in klist])            # __contains__()
        with test_support.check_py3k_warnings():
            self.assertEqual([cm.has_key(k) for k in klist],
                             [dcomb.has_key(k) for k in klist])  # has_key()

class Issue7005TestCase(unittest.TestCase):
    """Test output when None is set() as a value and allow_no_value == False.

    http://bugs.python.org/issue7005

    """

    expected_output = "[section]\noption = None\n\n"

    def prepare(self, config_class):
        # This is the default, but that's the point.
        cp = config_class(allow_no_value=False)
        cp.add_section("section")
        cp.set("section", "option", None)
        sio = StringIO.StringIO()
        cp.write(sio)
        return sio.getvalue()

    def test_none_as_value_stringified(self):
        output = self.prepare(ConfigParser.ConfigParser)
        self.assertEqual(output, self.expected_output)

    def test_none_as_value_stringified_raw(self):
        output = self.prepare(ConfigParser.RawConfigParser)
        self.assertEqual(output, self.expected_output)


class SortedTestCase(RawConfigParserTestCase):
    def newconfig(self, defaults=None):
        self.cf = self.config_class(defaults=defaults, dict_type=SortedDict)
        return self.cf

    def test_sorted(self):
        self.fromstring("[b]\n"
                        "o4=1\n"
                        "o3=2\n"
                        "o2=3\n"
                        "o1=4\n"
                        "[a]\n"
                        "k=v\n")
        output = StringIO.StringIO()
        self.cf.write(output)
        self.assertEqual(output.getvalue(),
                         "[a]\n"
                         "k = v\n\n"
                         "[b]\n"
                         "o1 = 4\n"
                         "o2 = 3\n"
                         "o3 = 2\n"
                         "o4 = 1\n\n")


class ExceptionPicklingTestCase(unittest.TestCase):
    """Tests for issue #13760: ConfigParser exceptions are not picklable."""

    def test_error(self):
        import pickle
        e1 = ConfigParser.Error('value')
        for proto in range(pickle.HIGHEST_PROTOCOL + 1):
            pickled = pickle.dumps(e1, proto)
            e2 = pickle.loads(pickled)
            self.assertEqual(e1.message, e2.message)
            self.assertEqual(repr(e1), repr(e2))

    def test_nosectionerror(self):
        import pickle
        e1 = ConfigParser.NoSectionError('section')
        for proto in range(pickle.HIGHEST_PROTOCOL + 1):
            pickled = pickle.dumps(e1, proto)
            e2 = pickle.loads(pickled)
            self.assertEqual(e1.message, e2.message)
            self.assertEqual(e1.args, e2.args)
            self.assertEqual(e1.section, e2.section)
            self.assertEqual(repr(e1), repr(e2))

    def test_nooptionerror(self):
        import pickle
        e1 = ConfigParser.NoOptionError('option', 'section')
        for proto in range(pickle.HIGHEST_PROTOCOL + 1):
            pickled = pickle.dumps(e1, proto)
            e2 = pickle.loads(pickled)
            self.assertEqual(e1.message, e2.message)
            self.assertEqual(e1.args, e2.args)
            self.assertEqual(e1.section, e2.section)
            self.assertEqual(e1.option, e2.option)
            self.assertEqual(repr(e1), repr(e2))

    def test_duplicatesectionerror(self):
        import pickle
        e1 = ConfigParser.DuplicateSectionError('section')
        for proto in range(pickle.HIGHEST_PROTOCOL + 1):
            pickled = pickle.dumps(e1, proto)
            e2 = pickle.loads(pickled)
            self.assertEqual(e1.message, e2.message)
            self.assertEqual(e1.args, e2.args)
            self.assertEqual(e1.section, e2.section)
            self.assertEqual(repr(e1), repr(e2))

    def test_interpolationerror(self):
        import pickle
        e1 = ConfigParser.InterpolationError('option', 'section', 'msg')
        for proto in range(pickle.HIGHEST_PROTOCOL + 1):
            pickled = pickle.dumps(e1, proto)
            e2 = pickle.loads(pickled)
            self.assertEqual(e1.message, e2.message)
            self.assertEqual(e1.args, e2.args)
            self.assertEqual(e1.section, e2.section)
            self.assertEqual(e1.option, e2.option)
            self.assertEqual(repr(e1), repr(e2))

    def test_interpolationmissingoptionerror(self):
        import pickle
        e1 = ConfigParser.InterpolationMissingOptionError('option', 'section',
            'rawval', 'reference')
        for proto in range(pickle.HIGHEST_PROTOCOL + 1):
            pickled = pickle.dumps(e1, proto)
            e2 = pickle.loads(pickled)
            self.assertEqual(e1.message, e2.message)
            self.assertEqual(e1.args, e2.args)
            self.assertEqual(e1.section, e2.section)
            self.assertEqual(e1.option, e2.option)
            self.assertEqual(e1.reference, e2.reference)
            self.assertEqual(repr(e1), repr(e2))

    def test_interpolationsyntaxerror(self):
        import pickle
        e1 = ConfigParser.InterpolationSyntaxError('option', 'section', 'msg')
        for proto in range(pickle.HIGHEST_PROTOCOL + 1):
            pickled = pickle.dumps(e1, proto)
            e2 = pickle.loads(pickled)
            self.assertEqual(e1.message, e2.message)
            self.assertEqual(e1.args, e2.args)
            self.assertEqual(e1.section, e2.section)
            self.assertEqual(e1.option, e2.option)
            self.assertEqual(repr(e1), repr(e2))

    def test_interpolationdeptherror(self):
        import pickle
        e1 = ConfigParser.InterpolationDepthError('option', 'section',
            'rawval')
        for proto in range(pickle.HIGHEST_PROTOCOL + 1):
            pickled = pickle.dumps(e1, proto)
            e2 = pickle.loads(pickled)
            self.assertEqual(e1.message, e2.message)
            self.assertEqual(e1.args, e2.args)
            self.assertEqual(e1.section, e2.section)
            self.assertEqual(e1.option, e2.option)
            self.assertEqual(repr(e1), repr(e2))

    def test_parsingerror(self):
        import pickle
        e1 = ConfigParser.ParsingError('source')
        e1.append(1, 'line1')
        e1.append(2, 'line2')
        e1.append(3, 'line3')
        for proto in range(pickle.HIGHEST_PROTOCOL + 1):
            pickled = pickle.dumps(e1, proto)
            e2 = pickle.loads(pickled)
            self.assertEqual(e1.message, e2.message)
            self.assertEqual(e1.args, e2.args)
            self.assertEqual(e1.filename, e2.filename)
            self.assertEqual(e1.errors, e2.errors)
            self.assertEqual(repr(e1), repr(e2))

    def test_missingsectionheadererror(self):
        import pickle
        e1 = ConfigParser.MissingSectionHeaderError('filename', 123, 'line')
        for proto in range(pickle.HIGHEST_PROTOCOL + 1):
            pickled = pickle.dumps(e1, proto)
            e2 = pickle.loads(pickled)
            self.assertEqual(e1.message, e2.message)
            self.assertEqual(e1.args, e2.args)
            self.assertEqual(e1.line, e2.line)
            self.assertEqual(e1.filename, e2.filename)
            self.assertEqual(e1.lineno, e2.lineno)
            self.assertEqual(repr(e1), repr(e2))


def test_main():
    test_support.run_unittest(
        ConfigParserTestCase,
        MultilineValuesTestCase,
        RawConfigParserTestCase,
        SafeConfigParserTestCase,
        SafeConfigParserTestCaseNoValue,
        SortedTestCase,
        Issue7005TestCase,
        TestChainMap,
        ExceptionPicklingTestCase,
        )


if __name__ == "__main__":
    test_main()
