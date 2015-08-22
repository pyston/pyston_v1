import os
import sys

sys.path.append(os.path.join(os.path.dirname(__file__), "../test/lib/pyxl/"))

from pyxl.codec.register import pyxl_transform_string

for i in xrange(100):
    # pyxl/tests/test_if_1.py
    pyxl_transform_string(
'''
from pyxl import html

def test():
    assert str(<frag><if cond="{True}">true</if><else>false</else></frag>) == "true"
    assert str(<frag><if cond="{False}">true</if><else>false</else></frag>) == "false"
''')

for i in xrange(100):
    # pyxl/tests/test_curlies_in_attrs_1.py
    pyxl_transform_string(
'''
from pyxl import html
def test():
    # kannan thinks this should be different
    assert str(<frag><img src="{'foo'}" /></frag>) == """<img src="foo" />"""
''')



for i in xrange(100):
    # pyxl/tests/test_rss.py
    pyxl_transform_string(
'''
import datetime

from  unittest2 import TestCase
from pyxl import html
from pyxl import rss

class RssTests(TestCase):
    def test_decl(self):
        decl = <rss.rss_decl_standalone />.to_string()
        self.assertEqual(decl, u'<?xml version="1.0" encoding="UTF-8" standalone="yes" ?>')

    def test_rss(self):
        r = <rss.rss version="2.0" />.to_string()
        self.assertEqual(r, u'<rss version="2.0"></rss>')

    def test_channel(self):
        c = (
            <rss.rss version="2.0">
                <rss.channel />
            </rss.rss>
        ).to_string()

        self.assertEqual(c, u'<rss version="2.0"><channel></channel></rss>')

    def test_channel_with_required_elements(self):
        channel = (
            <frag>
                <rss.rss_decl_standalone />
                <rss.rss version="2.0">
                    <rss.channel>
                        <rss.title>A Title</rss.title>
                        <rss.link>https://www.dropbox.com</rss.link>
                        <rss.description>A detailed description</rss.description>
                    </rss.channel>
                </rss.rss>
            </frag>
        )

        expected = """
<?xml version="1.0" encoding="UTF-8" standalone="yes" ?>
<rss version="2.0">
    <channel>
        <title>A Title</title>
        <link>https://www.dropbox.com</link>
        <description>A detailed description</description>
    </channel>
</rss>
"""
        expected = u''.join(l.strip() for l in expected.splitlines())

        self.assertEqual(channel.to_string(), expected)

    def test_channel_with_optional_elements(self):
        channel = (
            <frag>
                <rss.rss_decl_standalone />
                <rss.rss version="2.0">
                    <rss.channel>
                        <rss.title>A Title</rss.title>
                        <rss.link>https://www.dropbox.com</rss.link>
                        <rss.description>A detailed description</rss.description>
                        <rss.ttl>60</rss.ttl>
                        <rss.language>en-us</rss.language>
                    </rss.channel>
                </rss.rss>
            </frag>
        )

        expected = """
<?xml version="1.0" encoding="UTF-8" standalone="yes" ?>
<rss version="2.0">
    <channel>
        <title>A Title</title>
        <link>https://www.dropbox.com</link>
        <description>A detailed description</description>
        <ttl>60</ttl>
        <language>en-us</language>
    </channel>
</rss>
"""

        expected = u''.join(l.strip() for l in expected.splitlines())
        self.assertEqual(channel.to_string(), expected)

    def test_item_with_common_elements(self):
        item = (
            <rss.item>
                <rss.title>Item Title</rss.title>
                <rss.description>
                    {html.rawhtml('<![CDATA[ ')}
                    This is a really interesting description
                    {html.rawhtml(']]>')}
                </rss.description>
                <rss.link>https://www.dropbox.com/somewhere</rss.link>
            </rss.item>
        )

        expected = """
<item>
    <title>Item Title</title>
    <description><![CDATA[  This is a really interesting description ]]></description>
    <link>https://www.dropbox.com/somewhere</link>
</item>
"""

        expected = u''.join(l.strip() for l in expected.splitlines())
        self.assertEqual(item.to_string(), expected)

    def test_guid(self):
        self.assertEqual(<rss.guid>foo</rss.guid>.to_string(), u'<guid>foo</guid>')
        self.assertEqual(<rss.guid is-perma-link="{False}">foo</rss.guid>.to_string(), 
                         u'<guid isPermaLink="false">foo</guid>')
        self.assertEqual(<rss.guid is-perma-link="{True}">foo</rss.guid>.to_string(),
                         u'<guid isPermaLink="true">foo</guid>')

    def test_date_elements(self):
        dt = datetime.datetime(2013, 12, 17, 23, 54, 14)
        self.assertEqual(<rss.pubDate date="{dt}" />.to_string(),
                         u'<pubDate>Tue, 17 Dec 2013 23:54:14 GMT</pubDate>')
        self.assertEqual(<rss.lastBuildDate date="{dt}" />.to_string(),
                         u'<lastBuildDate>Tue, 17 Dec 2013 23:54:14 GMT</lastBuildDate>')

    def test_rss_document(self):
        dt = datetime.datetime(2013, 12, 17, 23, 54, 14)
        dt2 = datetime.datetime(2013, 12, 18, 11, 54, 14)
        doc = (
            <frag>
                <rss.rss_decl_standalone />
                <rss.rss version="2.0">
                    <rss.channel>
                        <rss.title>A Title</rss.title>
                        <rss.link>https://www.dropbox.com</rss.link>
                        <rss.description>A detailed description</rss.description>
                        <rss.ttl>60</rss.ttl>
                        <rss.language>en-us</rss.language>
                        <rss.lastBuildDate date="{dt}" />
                        <rss.item>
                            <rss.title>Item Title</rss.title>
                            <rss.description>
                                {html.rawhtml('<![CDATA[ ')}
                                This is a really interesting description
                                {html.rawhtml(']]>')}
                            </rss.description>
                            <rss.link>https://www.dropbox.com/somewhere</rss.link>
                            <rss.pubDate date="{dt}" />
                            <rss.guid is-perma-link="{False}">123456789</rss.guid>
                        </rss.item>
                        <rss.item>
                            <rss.title>Another Item</rss.title>
                            <rss.description>
                                {html.rawhtml('<![CDATA[ ')}
                                This is another really interesting description
                                {html.rawhtml(']]>')}
                            </rss.description>
                            <rss.link>https://www.dropbox.com/nowhere</rss.link>
                            <rss.pubDate date="{dt2}" />
                            <rss.guid is-perma-link="{False}">ABCDEFGHIJ</rss.guid>
                        </rss.item>
                    </rss.channel>
                </rss.rss>
            </frag>
        )

        expected = """
<?xml version="1.0" encoding="UTF-8" standalone="yes" ?>
<rss version="2.0">
    <channel>
        <title>A Title</title>
        <link>https://www.dropbox.com</link>
        <description>A detailed description</description>
        <ttl>60</ttl>
        <language>en-us</language>
        <lastBuildDate>Tue, 17 Dec 2013 23:54:14 GMT</lastBuildDate>
        <item>
            <title>Item Title</title>
            <description><![CDATA[  This is a really interesting description ]]></description>
            <link>https://www.dropbox.com/somewhere</link>
            <pubDate>Tue, 17 Dec 2013 23:54:14 GMT</pubDate>
            <guid isPermaLink="false">123456789</guid>
        </item>
        <item>
            <title>Another Item</title>
            <description><![CDATA[  This is another really interesting description ]]></description>
            <link>https://www.dropbox.com/nowhere</link>
            <pubDate>Wed, 18 Dec 2013 11:54:14 GMT</pubDate>
            <guid isPermaLink="false">ABCDEFGHIJ</guid>
        </item>
    </channel>
</rss>
"""

        expected = ''.join(l.strip() for l in expected.splitlines())

        self.assertEqual(doc.to_string(), expected)
''')
