"""
TestCases for exercising a Queue DB.
"""

import os, string
from pprint import pprint
import unittest

from test_all import db, verbose, get_new_database_path

#----------------------------------------------------------------------

class SimpleQueueTestCase(unittest.TestCase):
    def setUp(self):
        self.filename = get_new_database_path()

    def tearDown(self):
        try:
            os.remove(self.filename)
        except os.error:
            pass


    def test01_basic(self):
        # Basic Queue tests using the deprecated DBCursor.consume method.

        if verbose:
            print '\n', '-=' * 30
            print "Running %s.test01_basic..." % self.__class__.__name__

        d = db.DB()
        d.set_re_len(40)  # Queues must be fixed length
        d.open(self.filename, db.DB_QUEUE, db.DB_CREATE)

        if verbose:
            print "before appends" + '-' * 30
            pprint(d.stat())

        for x in string.letters:
            d.append(x * 40)

        self.assertEqual(len(d), len(string.letters))

        d.put(100, "some more data")
        d.put(101, "and some more ")
        d.put(75,  "out of order")
        d.put(1,   "replacement data")

        self.assertEqual(len(d), len(string.letters)+3)

        if verbose:
            print "before close" + '-' * 30
            pprint(d.stat())

        d.close()
        del d
        d = db.DB()
        d.open(self.filename)

        if verbose:
            print "after open" + '-' * 30
            pprint(d.stat())

        # Test "txn" as a positional parameter
        d.append("one more", None)
        # Test "txn" as a keyword parameter
        d.append("another one", txn=None)

        c = d.cursor()

        if verbose:
            print "after append" + '-' * 30
            pprint(d.stat())

        rec = c.consume()
        while rec:
            if verbose:
                print rec
            rec = c.consume()
        c.close()

        if verbose:
            print "after consume loop" + '-' * 30
            pprint(d.stat())

        self.assertEqual(len(d), 0, \
               "if you see this message then you need to rebuild " \
               "Berkeley DB 3.1.17 with the patch in patches/qam_stat.diff")

        d.close()



    def test02_basicPost32(self):
        # Basic Queue tests using the new DB.consume method in DB 3.2+
        # (No cursor needed)

        if verbose:
            print '\n', '-=' * 30
            print "Running %s.test02_basicPost32..." % self.__class__.__name__

        d = db.DB()
        d.set_re_len(40)  # Queues must be fixed length
        d.open(self.filename, db.DB_QUEUE, db.DB_CREATE)

        if verbose:
            print "before appends" + '-' * 30
            pprint(d.stat())

        for x in string.letters:
            d.append(x * 40)

        self.assertEqual(len(d), len(string.letters))

        d.put(100, "some more data")
        d.put(101, "and some more ")
        d.put(75,  "out of order")
        d.put(1,   "replacement data")

        self.assertEqual(len(d), len(string.letters)+3)

        if verbose:
            print "before close" + '-' * 30
            pprint(d.stat())

        d.close()
        del d
        d = db.DB()
        d.open(self.filename)
        #d.set_get_returns_none(true)

        if verbose:
            print "after open" + '-' * 30
            pprint(d.stat())

        d.append("one more")

        if verbose:
            print "after append" + '-' * 30
            pprint(d.stat())

        rec = d.consume()
        while rec:
            if verbose:
                print rec
            rec = d.consume()

        if verbose:
            print "after consume loop" + '-' * 30
            pprint(d.stat())

        d.close()



#----------------------------------------------------------------------

def test_suite():
    return unittest.makeSuite(SimpleQueueTestCase)


if __name__ == '__main__':
    unittest.main(defaultTest='test_suite')
