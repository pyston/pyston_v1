#-----------------------------------------------------------------------
# A test suite for the table interface built on bsddb.db
#-----------------------------------------------------------------------
#
# Copyright (C) 2000, 2001 by Autonomous Zone Industries
# Copyright (C) 2002 Gregory P. Smith
#
# March 20, 2000
#
# License:      This is free software.  You may use this software for any
#               purpose including modification/redistribution, so long as
#               this header remains intact and that you do not claim any
#               rights of ownership or authorship of this software.  This
#               software has been tested, but no warranty is expressed or
#               implied.
#
#   --  Gregory P. Smith <greg@krypto.org>
#
# $Id$

import os, re, sys

if sys.version_info[0] < 3 :
    try:
        import cPickle
        pickle = cPickle
    except ImportError:
        import pickle
else :
    import pickle

import unittest
from test_all import db, dbtables, test_support, verbose, \
        get_new_environment_path, get_new_database_path

#----------------------------------------------------------------------

class TableDBTestCase(unittest.TestCase):
    db_name = 'test-table.db'

    def setUp(self):
        import sys
        if sys.version_info[0] >= 3 :
            from test_all import do_proxy_db_py3k
            self._flag_proxy_db_py3k = do_proxy_db_py3k(False)

        self.testHomeDir = get_new_environment_path()
        self.tdb = dbtables.bsdTableDB(
            filename='tabletest.db', dbhome=self.testHomeDir, create=1)

    def tearDown(self):
        self.tdb.close()
        import sys
        if sys.version_info[0] >= 3 :
            from test_all import do_proxy_db_py3k
            do_proxy_db_py3k(self._flag_proxy_db_py3k)
        test_support.rmtree(self.testHomeDir)

    def test01(self):
        tabname = "test01"
        colname = 'cool numbers'
        try:
            self.tdb.Drop(tabname)
        except dbtables.TableDBError:
            pass
        self.tdb.CreateTable(tabname, [colname])
        import sys
        if sys.version_info[0] < 3 :
            self.tdb.Insert(tabname, {colname: pickle.dumps(3.14159, 1)})
        else :
            self.tdb.Insert(tabname, {colname: pickle.dumps(3.14159,
                1).decode("iso8859-1")})  # 8 bits

        if verbose:
            self.tdb._db_print()

        values = self.tdb.Select(
            tabname, [colname], conditions={colname: None})

        import sys
        if sys.version_info[0] < 3 :
            colval = pickle.loads(values[0][colname])
        else :
            colval = pickle.loads(bytes(values[0][colname], "iso8859-1"))
        self.assertTrue(colval > 3.141)
        self.assertTrue(colval < 3.142)


    def test02(self):
        tabname = "test02"
        col0 = 'coolness factor'
        col1 = 'but can it fly?'
        col2 = 'Species'

        import sys
        if sys.version_info[0] < 3 :
            testinfo = [
                {col0: pickle.dumps(8, 1), col1: 'no', col2: 'Penguin'},
                {col0: pickle.dumps(-1, 1), col1: 'no', col2: 'Turkey'},
                {col0: pickle.dumps(9, 1), col1: 'yes', col2: 'SR-71A Blackbird'}
            ]
        else :
            testinfo = [
                {col0: pickle.dumps(8, 1).decode("iso8859-1"),
                    col1: 'no', col2: 'Penguin'},
                {col0: pickle.dumps(-1, 1).decode("iso8859-1"),
                    col1: 'no', col2: 'Turkey'},
                {col0: pickle.dumps(9, 1).decode("iso8859-1"),
                    col1: 'yes', col2: 'SR-71A Blackbird'}
            ]

        try:
            self.tdb.Drop(tabname)
        except dbtables.TableDBError:
            pass
        self.tdb.CreateTable(tabname, [col0, col1, col2])
        for row in testinfo :
            self.tdb.Insert(tabname, row)

        import sys
        if sys.version_info[0] < 3 :
            values = self.tdb.Select(tabname, [col2],
                conditions={col0: lambda x: pickle.loads(x) >= 8})
        else :
            values = self.tdb.Select(tabname, [col2],
                conditions={col0: lambda x:
                    pickle.loads(bytes(x, "iso8859-1")) >= 8})

        self.assertEqual(len(values), 2)
        if values[0]['Species'] == 'Penguin' :
            self.assertEqual(values[1]['Species'], 'SR-71A Blackbird')
        elif values[0]['Species'] == 'SR-71A Blackbird' :
            self.assertEqual(values[1]['Species'], 'Penguin')
        else :
            if verbose:
                print "values= %r" % (values,)
            raise RuntimeError("Wrong values returned!")

    def test03(self):
        tabname = "test03"
        try:
            self.tdb.Drop(tabname)
        except dbtables.TableDBError:
            pass
        if verbose:
            print '...before CreateTable...'
            self.tdb._db_print()
        self.tdb.CreateTable(tabname, ['a', 'b', 'c', 'd', 'e'])
        if verbose:
            print '...after CreateTable...'
            self.tdb._db_print()
        self.tdb.Drop(tabname)
        if verbose:
            print '...after Drop...'
            self.tdb._db_print()
        self.tdb.CreateTable(tabname, ['a', 'b', 'c', 'd', 'e'])

        try:
            self.tdb.Insert(tabname,
                            {'a': "",
                             'e': pickle.dumps([{4:5, 6:7}, 'foo'], 1),
                             'f': "Zero"})
            self.fail('Expected an exception')
        except dbtables.TableDBError:
            pass

        try:
            self.tdb.Select(tabname, [], conditions={'foo': '123'})
            self.fail('Expected an exception')
        except dbtables.TableDBError:
            pass

        self.tdb.Insert(tabname,
                        {'a': '42',
                         'b': "bad",
                         'c': "meep",
                         'e': 'Fuzzy wuzzy was a bear'})
        self.tdb.Insert(tabname,
                        {'a': '581750',
                         'b': "good",
                         'd': "bla",
                         'c': "black",
                         'e': 'fuzzy was here'})
        self.tdb.Insert(tabname,
                        {'a': '800000',
                         'b': "good",
                         'd': "bla",
                         'c': "black",
                         'e': 'Fuzzy wuzzy is a bear'})

        if verbose:
            self.tdb._db_print()

        # this should return two rows
        values = self.tdb.Select(tabname, ['b', 'a', 'd'],
            conditions={'e': re.compile('wuzzy').search,
                        'a': re.compile('^[0-9]+$').match})
        self.assertEqual(len(values), 2)

        # now lets delete one of them and try again
        self.tdb.Delete(tabname, conditions={'b': dbtables.ExactCond('good')})
        values = self.tdb.Select(
            tabname, ['a', 'd', 'b'],
            conditions={'e': dbtables.PrefixCond('Fuzzy')})
        self.assertEqual(len(values), 1)
        self.assertEqual(values[0]['d'], None)

        values = self.tdb.Select(tabname, ['b'],
            conditions={'c': lambda c: c == 'meep'})
        self.assertEqual(len(values), 1)
        self.assertEqual(values[0]['b'], "bad")


    def test04_MultiCondSelect(self):
        tabname = "test04_MultiCondSelect"
        try:
            self.tdb.Drop(tabname)
        except dbtables.TableDBError:
            pass
        self.tdb.CreateTable(tabname, ['a', 'b', 'c', 'd', 'e'])

        try:
            self.tdb.Insert(tabname,
                            {'a': "",
                             'e': pickle.dumps([{4:5, 6:7}, 'foo'], 1),
                             'f': "Zero"})
            self.fail('Expected an exception')
        except dbtables.TableDBError:
            pass

        self.tdb.Insert(tabname, {'a': "A", 'b': "B", 'c': "C", 'd': "D",
                                  'e': "E"})
        self.tdb.Insert(tabname, {'a': "-A", 'b': "-B", 'c': "-C", 'd': "-D",
                                  'e': "-E"})
        self.tdb.Insert(tabname, {'a': "A-", 'b': "B-", 'c': "C-", 'd': "D-",
                                  'e': "E-"})

        if verbose:
            self.tdb._db_print()

        # This select should return 0 rows.  it is designed to test
        # the bug identified and fixed in sourceforge bug # 590449
        # (Big Thanks to "Rob Tillotson (n9mtb)" for tracking this down
        # and supplying a fix!!  This one caused many headaches to say
        # the least...)
        values = self.tdb.Select(tabname, ['b', 'a', 'd'],
            conditions={'e': dbtables.ExactCond('E'),
                        'a': dbtables.ExactCond('A'),
                        'd': dbtables.PrefixCond('-')
                       } )
        self.assertEqual(len(values), 0, values)


    def test_CreateOrExtend(self):
        tabname = "test_CreateOrExtend"

        self.tdb.CreateOrExtendTable(
            tabname, ['name', 'taste', 'filling', 'alcohol content', 'price'])
        try:
            self.tdb.Insert(tabname,
                            {'taste': 'crap',
                             'filling': 'no',
                             'is it Guinness?': 'no'})
            self.fail("Insert should've failed due to bad column name")
        except:
            pass
        self.tdb.CreateOrExtendTable(tabname,
                                     ['name', 'taste', 'is it Guinness?'])

        # these should both succeed as the table should contain the union of both sets of columns.
        self.tdb.Insert(tabname, {'taste': 'crap', 'filling': 'no',
                                  'is it Guinness?': 'no'})
        self.tdb.Insert(tabname, {'taste': 'great', 'filling': 'yes',
                                  'is it Guinness?': 'yes',
                                  'name': 'Guinness'})


    def test_CondObjs(self):
        tabname = "test_CondObjs"

        self.tdb.CreateTable(tabname, ['a', 'b', 'c', 'd', 'e', 'p'])

        self.tdb.Insert(tabname, {'a': "the letter A",
                                  'b': "the letter B",
                                  'c': "is for cookie"})
        self.tdb.Insert(tabname, {'a': "is for aardvark",
                                  'e': "the letter E",
                                  'c': "is for cookie",
                                  'd': "is for dog"})
        self.tdb.Insert(tabname, {'a': "the letter A",
                                  'e': "the letter E",
                                  'c': "is for cookie",
                                  'p': "is for Python"})

        values = self.tdb.Select(
            tabname, ['p', 'e'],
            conditions={'e': dbtables.PrefixCond('the l')})
        self.assertEqual(len(values), 2, values)
        self.assertEqual(values[0]['e'], values[1]['e'], values)
        self.assertNotEqual(values[0]['p'], values[1]['p'], values)

        values = self.tdb.Select(
            tabname, ['d', 'a'],
            conditions={'a': dbtables.LikeCond('%aardvark%')})
        self.assertEqual(len(values), 1, values)
        self.assertEqual(values[0]['d'], "is for dog", values)
        self.assertEqual(values[0]['a'], "is for aardvark", values)

        values = self.tdb.Select(tabname, None,
                                 {'b': dbtables.Cond(),
                                  'e':dbtables.LikeCond('%letter%'),
                                  'a':dbtables.PrefixCond('is'),
                                  'd':dbtables.ExactCond('is for dog'),
                                  'c':dbtables.PrefixCond('is for'),
                                  'p':lambda s: not s})
        self.assertEqual(len(values), 1, values)
        self.assertEqual(values[0]['d'], "is for dog", values)
        self.assertEqual(values[0]['a'], "is for aardvark", values)

    def test_Delete(self):
        tabname = "test_Delete"
        self.tdb.CreateTable(tabname, ['x', 'y', 'z'])

        # prior to 2001-05-09 there was a bug where Delete() would
        # fail if it encountered any rows that did not have values in
        # every column.
        # Hunted and Squashed by <Donwulff> (Jukka Santala - donwulff@nic.fi)
        self.tdb.Insert(tabname, {'x': 'X1', 'y':'Y1'})
        self.tdb.Insert(tabname, {'x': 'X2', 'y':'Y2', 'z': 'Z2'})

        self.tdb.Delete(tabname, conditions={'x': dbtables.PrefixCond('X')})
        values = self.tdb.Select(tabname, ['y'],
                                 conditions={'x': dbtables.PrefixCond('X')})
        self.assertEqual(len(values), 0)

    def test_Modify(self):
        tabname = "test_Modify"
        self.tdb.CreateTable(tabname, ['Name', 'Type', 'Access'])

        self.tdb.Insert(tabname, {'Name': 'Index to MP3 files.doc',
                                  'Type': 'Word', 'Access': '8'})
        self.tdb.Insert(tabname, {'Name': 'Nifty.MP3', 'Access': '1'})
        self.tdb.Insert(tabname, {'Type': 'Unknown', 'Access': '0'})

        def set_type(type):
            if type is None:
                return 'MP3'
            return type

        def increment_access(count):
            return str(int(count)+1)

        def remove_value(value):
            return None

        self.tdb.Modify(tabname,
                        conditions={'Access': dbtables.ExactCond('0')},
                        mappings={'Access': remove_value})
        self.tdb.Modify(tabname,
                        conditions={'Name': dbtables.LikeCond('%MP3%')},
                        mappings={'Type': set_type})
        self.tdb.Modify(tabname,
                        conditions={'Name': dbtables.LikeCond('%')},
                        mappings={'Access': increment_access})

        try:
            self.tdb.Modify(tabname,
                            conditions={'Name': dbtables.LikeCond('%')},
                            mappings={'Access': 'What is your quest?'})
        except TypeError:
            # success, the string value in mappings isn't callable
            pass
        else:
            raise RuntimeError, "why was TypeError not raised for bad callable?"

        # Delete key in select conditions
        values = self.tdb.Select(
            tabname, None,
            conditions={'Type': dbtables.ExactCond('Unknown')})
        self.assertEqual(len(values), 1, values)
        self.assertEqual(values[0]['Name'], None, values)
        self.assertEqual(values[0]['Access'], None, values)

        # Modify value by select conditions
        values = self.tdb.Select(
            tabname, None,
            conditions={'Name': dbtables.ExactCond('Nifty.MP3')})
        self.assertEqual(len(values), 1, values)
        self.assertEqual(values[0]['Type'], "MP3", values)
        self.assertEqual(values[0]['Access'], "2", values)

        # Make sure change applied only to select conditions
        values = self.tdb.Select(
            tabname, None, conditions={'Name': dbtables.LikeCond('%doc%')})
        self.assertEqual(len(values), 1, values)
        self.assertEqual(values[0]['Type'], "Word", values)
        self.assertEqual(values[0]['Access'], "9", values)


def test_suite():
    suite = unittest.TestSuite()
    suite.addTest(unittest.makeSuite(TableDBTestCase))
    return suite


if __name__ == '__main__':
    unittest.main(defaultTest='test_suite')
