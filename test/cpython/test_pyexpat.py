# XXX TypeErrors on calling handlers, or on bad return values from a
# handler, are obscure and unhelpful.

import StringIO, sys
import unittest

from xml.parsers import expat

from test import test_support
from test.test_support import sortdict, run_unittest


class SetAttributeTest(unittest.TestCase):
    def setUp(self):
        self.parser = expat.ParserCreate(namespace_separator='!')
        self.set_get_pairs = [
            [0, 0],
            [1, 1],
            [2, 1],
            [0, 0],
            ]

    def test_returns_unicode(self):
        for x, y in self.set_get_pairs:
            self.parser.returns_unicode = x
            self.assertEqual(self.parser.returns_unicode, y)

    def test_ordered_attributes(self):
        for x, y in self.set_get_pairs:
            self.parser.ordered_attributes = x
            self.assertEqual(self.parser.ordered_attributes, y)

    def test_specified_attributes(self):
        for x, y in self.set_get_pairs:
            self.parser.specified_attributes = x
            self.assertEqual(self.parser.specified_attributes, y)


data = '''\
<?xml version="1.0" encoding="iso-8859-1" standalone="no"?>
<?xml-stylesheet href="stylesheet.css"?>
<!-- comment data -->
<!DOCTYPE quotations SYSTEM "quotations.dtd" [
<!ELEMENT root ANY>
<!NOTATION notation SYSTEM "notation.jpeg">
<!ENTITY acirc "&#226;">
<!ENTITY external_entity SYSTEM "entity.file">
<!ENTITY unparsed_entity SYSTEM "entity.file" NDATA notation>
%unparsed_entity;
]>

<root attr1="value1" attr2="value2&#8000;">
<myns:subelement xmlns:myns="http://www.python.org/namespace">
     Contents of subelements
</myns:subelement>
<sub2><![CDATA[contents of CDATA section]]></sub2>
&external_entity;
</root>
'''


# Produce UTF-8 output
class ParseTest(unittest.TestCase):
    class Outputter:
        def __init__(self):
            self.out = []

        def StartElementHandler(self, name, attrs):
            self.out.append('Start element: ' + repr(name) + ' ' +
                            sortdict(attrs))

        def EndElementHandler(self, name):
            self.out.append('End element: ' + repr(name))

        def CharacterDataHandler(self, data):
            data = data.strip()
            if data:
                self.out.append('Character data: ' + repr(data))

        def ProcessingInstructionHandler(self, target, data):
            self.out.append('PI: ' + repr(target) + ' ' + repr(data))

        def StartNamespaceDeclHandler(self, prefix, uri):
            self.out.append('NS decl: ' + repr(prefix) + ' ' + repr(uri))

        def EndNamespaceDeclHandler(self, prefix):
            self.out.append('End of NS decl: ' + repr(prefix))

        def StartCdataSectionHandler(self):
            self.out.append('Start of CDATA section')

        def EndCdataSectionHandler(self):
            self.out.append('End of CDATA section')

        def CommentHandler(self, text):
            self.out.append('Comment: ' + repr(text))

        def NotationDeclHandler(self, *args):
            name, base, sysid, pubid = args
            self.out.append('Notation declared: %s' %(args,))

        def UnparsedEntityDeclHandler(self, *args):
            entityName, base, systemId, publicId, notationName = args
            self.out.append('Unparsed entity decl: %s' %(args,))

        def NotStandaloneHandler(self, userData):
            self.out.append('Not standalone')
            return 1

        def ExternalEntityRefHandler(self, *args):
            context, base, sysId, pubId = args
            self.out.append('External entity ref: %s' %(args[1:],))
            return 1

        def DefaultHandler(self, userData):
            pass

        def DefaultHandlerExpand(self, userData):
            pass

    handler_names = [
        'StartElementHandler', 'EndElementHandler',
        'CharacterDataHandler', 'ProcessingInstructionHandler',
        'UnparsedEntityDeclHandler', 'NotationDeclHandler',
        'StartNamespaceDeclHandler', 'EndNamespaceDeclHandler',
        'CommentHandler', 'StartCdataSectionHandler',
        'EndCdataSectionHandler',
        'DefaultHandler', 'DefaultHandlerExpand',
        #'NotStandaloneHandler',
        'ExternalEntityRefHandler'
        ]

    def test_utf8(self):

        out = self.Outputter()
        parser = expat.ParserCreate(namespace_separator='!')
        for name in self.handler_names:
            setattr(parser, name, getattr(out, name))
        parser.returns_unicode = 0
        parser.Parse(data, 1)

        # Verify output
        op = out.out
        self.assertEqual(op[0], 'PI: \'xml-stylesheet\' \'href="stylesheet.css"\'')
        self.assertEqual(op[1], "Comment: ' comment data '")
        self.assertEqual(op[2], "Notation declared: ('notation', None, 'notation.jpeg', None)")
        self.assertEqual(op[3], "Unparsed entity decl: ('unparsed_entity', None, 'entity.file', None, 'notation')")
        self.assertEqual(op[4], "Start element: 'root' {'attr1': 'value1', 'attr2': 'value2\\xe1\\xbd\\x80'}")
        self.assertEqual(op[5], "NS decl: 'myns' 'http://www.python.org/namespace'")
        self.assertEqual(op[6], "Start element: 'http://www.python.org/namespace!subelement' {}")
        self.assertEqual(op[7], "Character data: 'Contents of subelements'")
        self.assertEqual(op[8], "End element: 'http://www.python.org/namespace!subelement'")
        self.assertEqual(op[9], "End of NS decl: 'myns'")
        self.assertEqual(op[10], "Start element: 'sub2' {}")
        self.assertEqual(op[11], 'Start of CDATA section')
        self.assertEqual(op[12], "Character data: 'contents of CDATA section'")
        self.assertEqual(op[13], 'End of CDATA section')
        self.assertEqual(op[14], "End element: 'sub2'")
        self.assertEqual(op[15], "External entity ref: (None, 'entity.file', None)")
        self.assertEqual(op[16], "End element: 'root'")

    def test_unicode(self):
        # Try the parse again, this time producing Unicode output
        out = self.Outputter()
        parser = expat.ParserCreate(namespace_separator='!')
        parser.returns_unicode = 1
        for name in self.handler_names:
            setattr(parser, name, getattr(out, name))

        parser.Parse(data, 1)

        op = out.out
        self.assertEqual(op[0], 'PI: u\'xml-stylesheet\' u\'href="stylesheet.css"\'')
        self.assertEqual(op[1], "Comment: u' comment data '")
        self.assertEqual(op[2], "Notation declared: (u'notation', None, u'notation.jpeg', None)")
        self.assertEqual(op[3], "Unparsed entity decl: (u'unparsed_entity', None, u'entity.file', None, u'notation')")
        self.assertEqual(op[4], "Start element: u'root' {u'attr1': u'value1', u'attr2': u'value2\\u1f40'}")
        self.assertEqual(op[5], "NS decl: u'myns' u'http://www.python.org/namespace'")
        self.assertEqual(op[6], "Start element: u'http://www.python.org/namespace!subelement' {}")
        self.assertEqual(op[7], "Character data: u'Contents of subelements'")
        self.assertEqual(op[8], "End element: u'http://www.python.org/namespace!subelement'")
        self.assertEqual(op[9], "End of NS decl: u'myns'")
        self.assertEqual(op[10], "Start element: u'sub2' {}")
        self.assertEqual(op[11], 'Start of CDATA section')
        self.assertEqual(op[12], "Character data: u'contents of CDATA section'")
        self.assertEqual(op[13], 'End of CDATA section')
        self.assertEqual(op[14], "End element: u'sub2'")
        self.assertEqual(op[15], "External entity ref: (None, u'entity.file', None)")
        self.assertEqual(op[16], "End element: u'root'")

    def test_parse_file(self):
        # Try parsing a file
        out = self.Outputter()
        parser = expat.ParserCreate(namespace_separator='!')
        parser.returns_unicode = 1
        for name in self.handler_names:
            setattr(parser, name, getattr(out, name))
        file = StringIO.StringIO(data)

        parser.ParseFile(file)

        op = out.out
        self.assertEqual(op[0], 'PI: u\'xml-stylesheet\' u\'href="stylesheet.css"\'')
        self.assertEqual(op[1], "Comment: u' comment data '")
        self.assertEqual(op[2], "Notation declared: (u'notation', None, u'notation.jpeg', None)")
        self.assertEqual(op[3], "Unparsed entity decl: (u'unparsed_entity', None, u'entity.file', None, u'notation')")
        self.assertEqual(op[4], "Start element: u'root' {u'attr1': u'value1', u'attr2': u'value2\\u1f40'}")
        self.assertEqual(op[5], "NS decl: u'myns' u'http://www.python.org/namespace'")
        self.assertEqual(op[6], "Start element: u'http://www.python.org/namespace!subelement' {}")
        self.assertEqual(op[7], "Character data: u'Contents of subelements'")
        self.assertEqual(op[8], "End element: u'http://www.python.org/namespace!subelement'")
        self.assertEqual(op[9], "End of NS decl: u'myns'")
        self.assertEqual(op[10], "Start element: u'sub2' {}")
        self.assertEqual(op[11], 'Start of CDATA section')
        self.assertEqual(op[12], "Character data: u'contents of CDATA section'")
        self.assertEqual(op[13], 'End of CDATA section')
        self.assertEqual(op[14], "End element: u'sub2'")
        self.assertEqual(op[15], "External entity ref: (None, u'entity.file', None)")
        self.assertEqual(op[16], "End element: u'root'")

        # Issue 4877: expat.ParseFile causes segfault on a closed file.
        fp = open(test_support.TESTFN, 'wb')
        try:
            fp.close()
            parser = expat.ParserCreate()
            with self.assertRaises(ValueError):
                parser.ParseFile(fp)
        finally:
            test_support.unlink(test_support.TESTFN)

    def test_parse_again(self):
        parser = expat.ParserCreate()
        file = StringIO.StringIO(data)
        parser.ParseFile(file)
        # Issue 6676: ensure a meaningful exception is raised when attempting
        # to parse more than one XML document per xmlparser instance,
        # a limitation of the Expat library.
        with self.assertRaises(expat.error) as cm:
            parser.ParseFile(file)
        self.assertEqual(expat.ErrorString(cm.exception.code),
                          expat.errors.XML_ERROR_FINISHED)

class NamespaceSeparatorTest(unittest.TestCase):
    def test_legal(self):
        # Tests that make sure we get errors when the namespace_separator value
        # is illegal, and that we don't for good values:
        expat.ParserCreate()
        expat.ParserCreate(namespace_separator=None)
        expat.ParserCreate(namespace_separator=' ')

    def test_illegal(self):
        try:
            expat.ParserCreate(namespace_separator=42)
            self.fail()
        except TypeError, e:
            self.assertEqual(str(e),
                'ParserCreate() argument 2 must be string or None, not int')

        try:
            expat.ParserCreate(namespace_separator='too long')
            self.fail()
        except ValueError, e:
            self.assertEqual(str(e),
                'namespace_separator must be at most one character, omitted, or None')

    def test_zero_length(self):
        # ParserCreate() needs to accept a namespace_separator of zero length
        # to satisfy the requirements of RDF applications that are required
        # to simply glue together the namespace URI and the localname.  Though
        # considered a wart of the RDF specifications, it needs to be supported.
        #
        # See XML-SIG mailing list thread starting with
        # http://mail.python.org/pipermail/xml-sig/2001-April/005202.html
        #
        expat.ParserCreate(namespace_separator='') # too short


class InterningTest(unittest.TestCase):
    def test(self):
        # Test the interning machinery.
        p = expat.ParserCreate()
        L = []
        def collector(name, *args):
            L.append(name)
        p.StartElementHandler = collector
        p.EndElementHandler = collector
        p.Parse("<e> <e/> <e></e> </e>", 1)
        tag = L[0]
        self.assertEqual(len(L), 6)
        for entry in L:
            # L should have the same string repeated over and over.
            self.assertTrue(tag is entry)


class BufferTextTest(unittest.TestCase):
    def setUp(self):
        self.stuff = []
        self.parser = expat.ParserCreate()
        self.parser.buffer_text = 1
        self.parser.CharacterDataHandler = self.CharacterDataHandler

    def check(self, expected, label):
        self.assertEqual(self.stuff, expected,
                "%s\nstuff    = %r\nexpected = %r"
                % (label, self.stuff, map(unicode, expected)))

    def CharacterDataHandler(self, text):
        self.stuff.append(text)

    def StartElementHandler(self, name, attrs):
        self.stuff.append("<%s>" % name)
        bt = attrs.get("buffer-text")
        if bt == "yes":
            self.parser.buffer_text = 1
        elif bt == "no":
            self.parser.buffer_text = 0

    def EndElementHandler(self, name):
        self.stuff.append("</%s>" % name)

    def CommentHandler(self, data):
        self.stuff.append("<!--%s-->" % data)

    def setHandlers(self, handlers=[]):
        for name in handlers:
            setattr(self.parser, name, getattr(self, name))

    def test_default_to_disabled(self):
        parser = expat.ParserCreate()
        self.assertFalse(parser.buffer_text)

    def test_buffering_enabled(self):
        # Make sure buffering is turned on
        self.assertTrue(self.parser.buffer_text)
        self.parser.Parse("<a>1<b/>2<c/>3</a>", 1)
        self.assertEqual(self.stuff, ['123'],
                         "buffered text not properly collapsed")

    def test1(self):
        # XXX This test exposes more detail of Expat's text chunking than we
        # XXX like, but it tests what we need to concisely.
        self.setHandlers(["StartElementHandler"])
        self.parser.Parse("<a>1<b buffer-text='no'/>2\n3<c buffer-text='yes'/>4\n5</a>", 1)
        self.assertEqual(self.stuff,
                         ["<a>", "1", "<b>", "2", "\n", "3", "<c>", "4\n5"],
                         "buffering control not reacting as expected")

    def test2(self):
        self.parser.Parse("<a>1<b/>&lt;2&gt;<c/>&#32;\n&#x20;3</a>", 1)
        self.assertEqual(self.stuff, ["1<2> \n 3"],
                         "buffered text not properly collapsed")

    def test3(self):
        self.setHandlers(["StartElementHandler"])
        self.parser.Parse("<a>1<b/>2<c/>3</a>", 1)
        self.assertEqual(self.stuff, ["<a>", "1", "<b>", "2", "<c>", "3"],
                          "buffered text not properly split")

    def test4(self):
        self.setHandlers(["StartElementHandler", "EndElementHandler"])
        self.parser.CharacterDataHandler = None
        self.parser.Parse("<a>1<b/>2<c/>3</a>", 1)
        self.assertEqual(self.stuff,
                         ["<a>", "<b>", "</b>", "<c>", "</c>", "</a>"])

    def test5(self):
        self.setHandlers(["StartElementHandler", "EndElementHandler"])
        self.parser.Parse("<a>1<b></b>2<c/>3</a>", 1)
        self.assertEqual(self.stuff,
            ["<a>", "1", "<b>", "</b>", "2", "<c>", "</c>", "3", "</a>"])

    def test6(self):
        self.setHandlers(["CommentHandler", "EndElementHandler",
                    "StartElementHandler"])
        self.parser.Parse("<a>1<b/>2<c></c>345</a> ", 1)
        self.assertEqual(self.stuff,
            ["<a>", "1", "<b>", "</b>", "2", "<c>", "</c>", "345", "</a>"],
            "buffered text not properly split")

    def test7(self):
        self.setHandlers(["CommentHandler", "EndElementHandler",
                    "StartElementHandler"])
        self.parser.Parse("<a>1<b/>2<c></c>3<!--abc-->4<!--def-->5</a> ", 1)
        self.assertEqual(self.stuff,
                         ["<a>", "1", "<b>", "</b>", "2", "<c>", "</c>", "3",
                          "<!--abc-->", "4", "<!--def-->", "5", "</a>"],
                         "buffered text not properly split")


# Test handling of exception from callback:
class HandlerExceptionTest(unittest.TestCase):
    def StartElementHandler(self, name, attrs):
        raise RuntimeError(name)

    def test(self):
        parser = expat.ParserCreate()
        parser.StartElementHandler = self.StartElementHandler
        try:
            parser.Parse("<a><b><c/></b></a>", 1)
            self.fail()
        except RuntimeError, e:
            self.assertEqual(e.args[0], 'a',
                             "Expected RuntimeError for element 'a', but" + \
                             " found %r" % e.args[0])


# Test Current* members:
class PositionTest(unittest.TestCase):
    def StartElementHandler(self, name, attrs):
        self.check_pos('s')

    def EndElementHandler(self, name):
        self.check_pos('e')

    def check_pos(self, event):
        pos = (event,
               self.parser.CurrentByteIndex,
               self.parser.CurrentLineNumber,
               self.parser.CurrentColumnNumber)
        self.assertTrue(self.upto < len(self.expected_list),
                        'too many parser events')
        expected = self.expected_list[self.upto]
        self.assertEqual(pos, expected,
                'Expected position %s, got position %s' %(pos, expected))
        self.upto += 1

    def test(self):
        self.parser = expat.ParserCreate()
        self.parser.StartElementHandler = self.StartElementHandler
        self.parser.EndElementHandler = self.EndElementHandler
        self.upto = 0
        self.expected_list = [('s', 0, 1, 0), ('s', 5, 2, 1), ('s', 11, 3, 2),
                              ('e', 15, 3, 6), ('e', 17, 4, 1), ('e', 22, 5, 0)]

        xml = '<a>\n <b>\n  <c/>\n </b>\n</a>'
        self.parser.Parse(xml, 1)


class sf1296433Test(unittest.TestCase):
    def test_parse_only_xml_data(self):
        # http://python.org/sf/1296433
        #
        xml = "<?xml version='1.0' encoding='iso8859'?><s>%s</s>" % ('a' * 1025)
        # this one doesn't crash
        #xml = "<?xml version='1.0'?><s>%s</s>" % ('a' * 10000)

        class SpecificException(Exception):
            pass

        def handler(text):
            raise SpecificException

        parser = expat.ParserCreate()
        parser.CharacterDataHandler = handler

        self.assertRaises(Exception, parser.Parse, xml)

class ChardataBufferTest(unittest.TestCase):
    """
    test setting of chardata buffer size
    """

    def test_1025_bytes(self):
        self.assertEqual(self.small_buffer_test(1025), 2)

    def test_1000_bytes(self):
        self.assertEqual(self.small_buffer_test(1000), 1)

    def test_wrong_size(self):
        parser = expat.ParserCreate()
        parser.buffer_text = 1
        def f(size):
            parser.buffer_size = size

        self.assertRaises(TypeError, f, sys.maxint+1)
        self.assertRaises(ValueError, f, -1)
        self.assertRaises(ValueError, f, 0)

    def test_unchanged_size(self):
        xml1 = ("<?xml version='1.0' encoding='iso8859'?><s>%s" % ('a' * 512))
        xml2 = 'a'*512 + '</s>'
        parser = expat.ParserCreate()
        parser.CharacterDataHandler = self.counting_handler
        parser.buffer_size = 512
        parser.buffer_text = 1

        # Feed 512 bytes of character data: the handler should be called
        # once.
        self.n = 0
        parser.Parse(xml1)
        self.assertEqual(self.n, 1)

        # Reassign to buffer_size, but assign the same size.
        parser.buffer_size = parser.buffer_size
        self.assertEqual(self.n, 1)

        # Try parsing rest of the document
        parser.Parse(xml2)
        self.assertEqual(self.n, 2)


    def test_disabling_buffer(self):
        xml1 = "<?xml version='1.0' encoding='iso8859'?><a>%s" % ('a' * 512)
        xml2 = ('b' * 1024)
        xml3 = "%s</a>" % ('c' * 1024)
        parser = expat.ParserCreate()
        parser.CharacterDataHandler = self.counting_handler
        parser.buffer_text = 1
        parser.buffer_size = 1024
        self.assertEqual(parser.buffer_size, 1024)

        # Parse one chunk of XML
        self.n = 0
        parser.Parse(xml1, 0)
        self.assertEqual(parser.buffer_size, 1024)
        self.assertEqual(self.n, 1)

        # Turn off buffering and parse the next chunk.
        parser.buffer_text = 0
        self.assertFalse(parser.buffer_text)
        self.assertEqual(parser.buffer_size, 1024)
        for i in range(10):
            parser.Parse(xml2, 0)
        self.assertEqual(self.n, 11)

        parser.buffer_text = 1
        self.assertTrue(parser.buffer_text)
        self.assertEqual(parser.buffer_size, 1024)
        parser.Parse(xml3, 1)
        self.assertEqual(self.n, 12)



    def make_document(self, bytes):
        return ("<?xml version='1.0'?><tag>" + bytes * 'a' + '</tag>')

    def counting_handler(self, text):
        self.n += 1

    def small_buffer_test(self, buffer_len):
        xml = "<?xml version='1.0' encoding='iso8859'?><s>%s</s>" % ('a' * buffer_len)
        parser = expat.ParserCreate()
        parser.CharacterDataHandler = self.counting_handler
        parser.buffer_size = 1024
        parser.buffer_text = 1

        self.n = 0
        parser.Parse(xml)
        return self.n

    def test_change_size_1(self):
        xml1 = "<?xml version='1.0' encoding='iso8859'?><a><s>%s" % ('a' * 1024)
        xml2 = "aaa</s><s>%s</s></a>" % ('a' * 1025)
        parser = expat.ParserCreate()
        parser.CharacterDataHandler = self.counting_handler
        parser.buffer_text = 1
        parser.buffer_size = 1024
        self.assertEqual(parser.buffer_size, 1024)

        self.n = 0
        parser.Parse(xml1, 0)
        parser.buffer_size *= 2
        self.assertEqual(parser.buffer_size, 2048)
        parser.Parse(xml2, 1)
        self.assertEqual(self.n, 2)

    def test_change_size_2(self):
        xml1 = "<?xml version='1.0' encoding='iso8859'?><a>a<s>%s" % ('a' * 1023)
        xml2 = "aaa</s><s>%s</s></a>" % ('a' * 1025)
        parser = expat.ParserCreate()
        parser.CharacterDataHandler = self.counting_handler
        parser.buffer_text = 1
        parser.buffer_size = 2048
        self.assertEqual(parser.buffer_size, 2048)

        self.n=0
        parser.Parse(xml1, 0)
        parser.buffer_size //= 2
        self.assertEqual(parser.buffer_size, 1024)
        parser.Parse(xml2, 1)
        self.assertEqual(self.n, 4)

class MalformedInputText(unittest.TestCase):
    def test1(self):
        xml = "\0\r\n"
        parser = expat.ParserCreate()
        try:
            parser.Parse(xml, True)
            self.fail()
        except expat.ExpatError as e:
            self.assertEqual(str(e), 'unclosed token: line 2, column 0')

    def test2(self):
        xml = "<?xml version\xc2\x85='1.0'?>\r\n"
        parser = expat.ParserCreate()
        try:
            parser.Parse(xml, True)
            self.fail()
        except expat.ExpatError as e:
            self.assertEqual(str(e), 'XML declaration not well-formed: line 1, column 14')

class ForeignDTDTests(unittest.TestCase):
    """
    Tests for the UseForeignDTD method of expat parser objects.
    """
    def test_use_foreign_dtd(self):
        """
        If UseForeignDTD is passed True and a document without an external
        entity reference is parsed, ExternalEntityRefHandler is first called
        with None for the public and system ids.
        """
        handler_call_args = []
        def resolve_entity(context, base, system_id, public_id):
            handler_call_args.append((public_id, system_id))
            return 1

        parser = expat.ParserCreate()
        parser.UseForeignDTD(True)
        parser.SetParamEntityParsing(expat.XML_PARAM_ENTITY_PARSING_ALWAYS)
        parser.ExternalEntityRefHandler = resolve_entity
        parser.Parse("<?xml version='1.0'?><element/>")
        self.assertEqual(handler_call_args, [(None, None)])

        # test UseForeignDTD() is equal to UseForeignDTD(True)
        handler_call_args[:] = []

        parser = expat.ParserCreate()
        parser.UseForeignDTD()
        parser.SetParamEntityParsing(expat.XML_PARAM_ENTITY_PARSING_ALWAYS)
        parser.ExternalEntityRefHandler = resolve_entity
        parser.Parse("<?xml version='1.0'?><element/>")
        self.assertEqual(handler_call_args, [(None, None)])

    def test_ignore_use_foreign_dtd(self):
        """
        If UseForeignDTD is passed True and a document with an external
        entity reference is parsed, ExternalEntityRefHandler is called with
        the public and system ids from the document.
        """
        handler_call_args = []
        def resolve_entity(context, base, system_id, public_id):
            handler_call_args.append((public_id, system_id))
            return 1

        parser = expat.ParserCreate()
        parser.UseForeignDTD(True)
        parser.SetParamEntityParsing(expat.XML_PARAM_ENTITY_PARSING_ALWAYS)
        parser.ExternalEntityRefHandler = resolve_entity
        parser.Parse(
            "<?xml version='1.0'?><!DOCTYPE foo PUBLIC 'bar' 'baz'><element/>")
        self.assertEqual(handler_call_args, [("bar", "baz")])


def test_main():
    run_unittest(SetAttributeTest,
                 ParseTest,
                 NamespaceSeparatorTest,
                 InterningTest,
                 BufferTextTest,
                 HandlerExceptionTest,
                 PositionTest,
                 sf1296433Test,
                 ChardataBufferTest,
                 MalformedInputText,
                 ForeignDTDTests)

if __name__ == "__main__":
    test_main()
