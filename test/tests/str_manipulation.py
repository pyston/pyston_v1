import sys

# This group of tests are dedicated for Agata D. - my ex-girlfriend.
# Sorry, I promise it to her :P

#class Sequence(object):
    #def __init__(self, seq='wxyz'): self.seq = seq
    #def __len__(self): return len(self.seq)
    #def __getitem__(self, i): return self.seq[i]

#class BadSeq1(Sequence):
    #def __init__(self): self.seq = [7, 'hello', 123L]

#class BadSeq2(Sequence):
    #def __init__(self): self.seq = ['a', 'b', 'c']
    #def __len__(self): return 8


class CommonTest(object):
    def test_capitalize(self):
        var = ' hello '
        assert var.capitalize() == ' hello '

        var = 'Hello '
        assert var.capitalize() == 'Hello '

        var = 'hello '
        assert var.capitalize() == 'Hello '

        var = 'aaaa'
        assert var.capitalize() == 'Aaaa'

        var = 'AaAa'
        assert var.capitalize() == 'Aaaa'

        # TODO: crashes Pyston
        #var = 'hello'
        #try:
        #    var.capitalize(42)
        #    assert 'Should throw TypeError: capitalize() takes no arguments (1 given)' == False
        #except TypeError:
        #    assert True

    def test_count(self):
        var = 'aaa'
        assert var.count('a') == 3
        assert var.count('b') == 0
        assert var.count('a') == 3
        assert var.count('b') == 0
        assert var.count('a') == 3
        assert var.count('b') == 0
        assert var.count('a', 1) == 2
        assert var.count('a', 10) == 0
        assert var.count('a', -1) == 1
        assert var.count('a', -10) == 3
        assert var.count('a', 0, 1) == 1
        assert var.count('a', 0, 10) == 3
        assert var.count('a', 0, -1) == 2
        assert var.count('a', 0, -10) == 0
        assert var.count('', 1) == 3
        assert var.count('', 3) == 1
        assert var.count('', 10) == 0
        assert var.count('', -1) == 2
        assert var.count('', -10) == 4

        var = ''
        assert var.count('') == 1
        assert var.count('', 1, 1) == 0
        # TODO: doesnt work yet...
        #assert var.count('', sys.maxint, 0) == 0 #self.checkequal(0, '', 'count', '', sys.maxint, 0)
        assert var.count('xx') == 0
        assert var.count('xx', 1, 1) == 0
        assert var.count('xx', sys.maxint, 0) == 0 #self.checkequal(0, '', 'count', 'xx', sys.maxint, 0)

        var = 'hello'
        try:
            var.count()
            assert 'Should throw TypeError: count() takes at least 1 argument (0 given)' == False
        except TypeError:
            assert True

        var = 'hello'
        try:
            var.count(42)
            assert 'Should throw TypeError: expected a character buffer object' == False
        except TypeError:
            assert True

        ## For a variety of combinations,
        ##    verify that str.count() matches an equivalent function
        ##    replacing all occurrences and then differencing the string lengths
        #charset = ['', 'a', 'b']
        #digits = 7
        #base = len(charset)
        #teststrings = set()
        #for i in xrange(base ** digits):
            #entry = []
            #for j in xrange(digits):
                #i, m = divmod(i, base)
                #entry.append(charset[m])
            #teststrings.add(''.join(entry))
        #teststrings = list(teststrings)
        #for i in teststrings:
            #i = self.fixtype(i)
            #n = len(i)
            #for j in teststrings:
                #r1 = i.count(j)
                #if j:
                    #r2, rem = divmod(n - len(i.replace(j, '')), len(j))
                #else:
                    #r2, rem = len(i)+1, 0
                #if rem or r1 != r2:
                    #self.assertEqual(rem, 0, '%s != 0 for %s' % (rem, i))
                    #self.assertEqual(r1, r2, '%s != %s for %s' % (r1, r2, i))

    def test_find(self):
        var = 'abcdefghiabc'
        assert var.find('abc') == 0 #self.checkequal(0, 'abcdefghiabc', 'find', 'abc')
        var = 'abcdefghiabc'
        assert var.find('abc', 1) == 9 #self.checkequal(9, 'abcdefghiabc', 'find', 'abc', 1)
        var = 'abcdefghiabc'
        assert var.find('def', 4) == -1 #self.checkequal(-1, 'abcdefghiabc', 'find', 'def', 4)

        var = 'abc'
        assert var.find('', 0) == 0 #self.checkequal(0, 'abc', 'find', '', 0)
        var = 'abc'
        assert var.find('', 3) == 3 #self.checkequal(3, 'abc', 'find', '', 3)
        var = 'abc'
        assert var.find('', 4) == -1 #self.checkequal(-1, 'abc', 'find', '', 4)

        ## to check the ability to pass None as defaults
        var = 'rrarrrrrrrrra'
        assert var.find('a') ==  2 #self.checkequal( 2, 'rrarrrrrrrrra', 'find', 'a')
        var = 'rrarrrrrrrrra'
        assert var.find('a', 4) == 12 #self.checkequal(12, 'rrarrrrrrrrra', 'find', 'a', 4)
        var = 'rrarrrrrrrrra'
        assert var.find('a', 4, 6) == -1 #self.checkequal(-1, 'rrarrrrrrrrra', 'find', 'a', 4, 6)
        var = 'rrarrrrrrrrra'
        assert var.find('a', 4, None) == 12 #self.checkequal(12, 'rrarrrrrrrrra', 'find', 'a', 4, None)
        var = 'rrarrrrrrrrra'
        assert var.find('a', None, 6) ==  2 #self.checkequal( 2, 'rrarrrrrrrrra', 'find', 'a', None, 6)

        var = 'hello'
        try:
            var.find()
            assert 'Should throw TypeError: find() takes at least 1 argument (0 given)' == False
        except TypeError:
            assert True

        var = 'hello'
        try:
            var.find(42)
            assert 'Should throw TypeError: expected a character buffer object' == False
        except TypeError:
            assert True

        var = ''
        assert var.find('') == 0 #self.checkequal(0, '', 'find', '')
        var = ''
        assert var.find('', 1, 1) == -1 #self.checkequal(-1, '', 'find', '', 1, 1)
        # TODO: this is not working yet...
        #var = ''
        #assert var.find('', sys.maxint, 0) == -1 #self.checkequal(-1, '', 'find', '', sys.maxint, 0)

        var = ''
        assert var.find('xx') == -1 #self.checkequal(-1, '', 'find', 'xx')
        var = ''
        assert var.find('xx', 1, 1) == -1 #self.checkequal(-1, '', 'find', 'xx', 1, 1)
        var = ''
        assert var.find('xx', sys.maxint, 0) == -1 #self.checkequal(-1, '', 'find', 'xx', sys.maxint, 0)

        var = 'ab'
        assert var.find('xxx', sys.maxsize + 1, 0) == -1 #self.checkequal(-1, 'ab', 'find', 'xxx', sys.maxsize + 1, 0)

        ## For a variety of combinations,
        ##    verify that str.find() matches __contains__
        ##    and that the found substring is really at that location
        #charset = ['', 'a', 'b', 'c']
        #digits = 5
        #base = len(charset)
        #teststrings = set()
        #for i in xrange(base ** digits):
            #entry = []
            #for j in xrange(digits):
                #i, m = divmod(i, base)
                #entry.append(charset[m])
            #teststrings.add(''.join(entry))
        #teststrings = list(teststrings)
        #for i in teststrings:
            #i = self.fixtype(i)
            #for j in teststrings:
                #loc = i.find(j)
                #r1 = (loc != -1)
                #r2 = j in i
                #self.assertEqual(r1, r2)
                #if loc != -1:
                    #self.assertEqual(i[loc:loc+len(j)], j)

    def test_rfind(self):
        var = 'abcdefghiabc'
        assert var.rfind('abc') == 9 #self.checkequal(9,  'abcdefghiabc', 'rfind', 'abc')
        var = 'abcdefghiabc'
        assert var.rfind('') == 12 #self.checkequal(12, 'abcdefghiabc', 'rfind', '')
        var = 'abcdefghiabc'
        assert var.rfind('abcd') == 0 #self.checkequal(0, 'abcdefghiabc', 'rfind', 'abcd')
        var = 'abcdefghiabc'
        assert var.rfind('abcz') == -1 #self.checkequal(-1, 'abcdefghiabc', 'rfind', 'abcz')

        var = 'abc'
        assert var.rfind('', 0) == 3 #self.checkequal(3, 'abc', 'rfind', '', 0)
        var = 'abc'
        assert var.rfind('', 3) == 3 #self.checkequal(3, 'abc', 'rfind', '', 3)
        var = 'abc'
        assert var.rfind('', 4) == -1 #self.checkequal(-1, 'abc', 'rfind', '', 4)

        ## to check the ability to pass None as defaults
        var = 'rrarrrrrrrrra'
        assert var.rfind('a') == 12
        assert var.rfind('a', 4) == 12
        assert var.rfind('a', 4, 6) == -1
        assert var.rfind('a', 4, None) == 12
        assert var.rfind('a', None, 6) ==  2

        var = 'hello'
        try:
            var.rfind()
            assert 'Should throw TypeError: rfind() takes at least 1 argument (0 given)' == False
        except TypeError:
            assert True

        var = 'hello'
        try:
            var.rfind(42)
            assert 'Should throw TypeError: expected a character buffer object' == False
        except TypeError:
            assert True

        ## For a variety of combinations,
        ##    verify that str.rfind() matches __contains__
        ##    and that the found substring is really at that location
        #charset = ['', 'a', 'b', 'c']
        #digits = 5
        #base = len(charset)
        #teststrings = set()
        #for i in xrange(base ** digits):
            #entry = []
            #for j in xrange(digits):
                #i, m = divmod(i, base)
                #entry.append(charset[m])
            #teststrings.add(''.join(entry))
        #teststrings = list(teststrings)
        #for i in teststrings:
            #i = self.fixtype(i)
            #for j in teststrings:
                #loc = i.rfind(j)
                #r1 = (loc != -1)
                #r2 = j in i
                #self.assertEqual(r1, r2)
                #if loc != -1:
                    #self.assertEqual(i[loc:loc+len(j)], self.fixtype(j))

        var = 'ab'
        assert var.rfind('xxx', sys.maxsize + 1, 0) == -1 #self.checkequal(-1, 'ab', 'rfind', 'xxx', sys.maxsize + 1, 0)

    def test_index(self):
        var = 'abcdefghiabc'
        assert var.index('') == 0
        assert var.index('def') == 3
        assert var.index('abc') == 0
        assert var.index('abc', 1) == 9

        var = 'abcdefghiabc'
        try:
            var.index('hib')
            assert 'Should throw ValueError: substring not found' == False
        except ValueError:
            assert True

        var = 'abcdefghiab'
        try:
            var.index('abc', 1)
            assert 'Should throw ValueError: substring not found' == False
        except ValueError:
            assert True

        var = 'abcdefghi'
        try:
            var.index('ghi', 8)
            assert 'Should throw ValueError: substring not found' == False
        except ValueError:
            assert True

        ## to check the ability to pass None as defaults
        var = 'rrarrrrrrrrra'
        assert var.index('a') ==  2
        assert var.index('a', 4) == 12
        assert var.index('a', 4, None) == 12
        assert var.index('a', None, 6) ==  2

        var = 'rrarrrrrrrrra'
        try:
            var.index(4, 6)
            assert 'Should throw TypeError: expected a character buffer object' == False
        except TypeError:
            assert True

        var = 'hello'
        try:
            var.index()
            assert 'Should throw TypeError: index() takes at least 1 argument (0 given)' == False
        except TypeError:
            assert True

        var = 'hello'
        try:
            var.index(42)
            assert 'Should throw TypeError: expected a character buffer object' == False
        except TypeError:
            assert True

    def test_rindex(self):
        var = 'abcdefghiabc'
        assert var.rindex('') == 12 #self.checkequal(12, 'abcdefghiabc', 'rindex', '')
        var = 'abcdefghiabc'
        assert var.rindex('def') == 3 #self.checkequal(3, 'abcdefghiabc', 'rindex', 'def')
        var = 'abcdefghiabc'
        assert var.rindex('abc') == 9 #self.checkequal(9, 'abcdefghiabc', 'rindex', 'abc')
        var = 'abcdefghiabc'
        # TODO: doesn't work with pyston yet
        #assert var.rindex('abc', 0, -1) == 0 #self.checkequal(0, 'abcdefghiabc', 'rindex', 'abc', 0, -1)

        #self.checkraises(ValueError, 'abcdefghiabc', 'rindex', 'hib')
        #self.checkraises(ValueError, 'defghiabc', 'rindex', 'def', 1)
        #self.checkraises(ValueError, 'defghiabc', 'rindex', 'abc', 0, -1)
        #self.checkraises(ValueError, 'abcdefghi', 'rindex', 'ghi', 0, 8)
        #self.checkraises(ValueError, 'abcdefghi', 'rindex', 'ghi', 0, -1)

        ## to check the ability to pass None as defaults
        var = 'rrarrrrrrrrra'
        assert var.rindex('a') == 12
        assert var.rindex('a', 4) == 12
        assert var.rindex('a', 4, None) == 12
        assert var.rindex('a', None, 6) ==  2

        var = 'rrarrrrrrrrra'
        try:
            var.rindex('a', 4, 6)
            assert 'Should throw ValueError: substring not found' == False
        except ValueError:
            assert True

        var = 'hello'
        try:
            var.rindex()
            assert 'Should throw TypeError: rindex() takes at least 1 argument (0 given)' == False
        except TypeError:
            assert True

        var = 'hello'
        try:
            var.rindex(42)
            assert 'Should throw TypeError: expected a character buffer object' == False
        except TypeError:
            assert True

    def test_lower(self):
        var = 'HeLLo'
        assert var.lower() == 'hello'
        var = 'hello'
        assert var.lower() == 'hello'
        #self.checkraises(TypeError, 'hello', 'lower', 42)

    def test_upper(self):
        var = 'HeLLo'
        assert var.upper() == 'HELLO'
        var = 'HELLO'
        assert var.upper() == 'HELLO'
        #self.checkraises(TypeError, 'hello', 'upper', 42)

    def test_expandtabs(self):
        var = 'abc\rab\tdef\ng\thi'
        assert var.expandtabs() == 'abc\rab      def\ng       hi'

        var = 'abc\rab\tdef\ng\thi'
        assert var.expandtabs(8) == 'abc\rab      def\ng       hi'

        var = 'abc\rab\tdef\ng\thi'
        assert var.expandtabs(4) == 'abc\rab  def\ng   hi'

        var = 'abc\r\nab\tdef\ng\thi'
        assert var.expandtabs(4) == 'abc\r\nab  def\ng   hi'

        var = 'abc\rab\tdef\ng\thi'
        assert var.expandtabs() == 'abc\rab      def\ng       hi'

        var = 'abc\rab\tdef\ng\thi'
        assert var.expandtabs(8) == 'abc\rab      def\ng       hi'

        var = 'abc\r\nab\r\ndef\ng\r\nhi'
        assert var.expandtabs(4) == 'abc\r\nab\r\ndef\ng\r\nhi'

        var = ' \ta\n\tb'
        assert var.expandtabs(1) == '  a\n b'

        #self.checkraises(TypeError, 'hello', 'expandtabs', 42, 42)
        ## This test is only valid when sizeof(int) == sizeof(void*) == 4.
        #if sys.maxint < (1 << 32) and struct.calcsize('P') == 4:
            #self.checkraises(OverflowError,
                             #'\ta\n\tb', 'expandtabs', sys.maxint)

    def test_split(self):
        var = 'this is the split function'
        assert var.split() == ['this', 'is', 'the', 'split', 'function']

        # by whitespace
        var = 'a b c d '
        assert var.split() == ['a', 'b', 'c', 'd']

        var = 'a b c d'
        assert var.split(None, 1) == ['a', 'b c d']
        assert var.split(None, 2) == ['a', 'b', 'c d']
        assert var.split(None, 3) == ['a', 'b', 'c', 'd']
        assert var.split(None, 4) == ['a', 'b', 'c', 'd']
        assert var.split(None, sys.maxint-1) == ['a', 'b', 'c', 'd']

        assert var.split(None, 0) == ['a b c d']
        var = '  a b c d'
        assert var.split(None, 0) == ['a b c d']

        var = 'a  b  c  d'
        assert var.split(None, 2) == ['a', 'b', 'c  d']

        var = '         '
        assert var.split() == []

        var = '  a    '
        assert var.split() == ['a']

        var = '  a    b   '
        assert var.split() == ['a', 'b']

        var = '  a    b   '
        assert var.split(None, 1) == ['a', 'b   ']

        var = '  a    b   c   '
        assert var.split(None, 1) == ['a', 'b   c   ']

        var = '  a    b   c   '
        assert var.split(None, 2) == ['a', 'b', 'c   ']

        var = '\n\ta \t\r b \v '
        assert var.split() == ['a', 'b']

        aaa = ' a '*20
        assert aaa.split() == ['a']*20
        assert aaa.split(None, 1) == ['a'] + [aaa[4:]]
        assert aaa.split(None, 19) == ['a']*19 + ['a ']

        # by a char
        var = 'a|b|c|d'
        assert var.split('|') == ['a', 'b', 'c', 'd']

        var = 'a|b|c|d'
        assert var.split('|', 0) == ['a|b|c|d']

        var = 'a|b|c|d'
        assert var.split('|', 1) == ['a', 'b|c|d']

        var = 'a|b|c|d'
        assert var.split('|', 2) == ['a', 'b', 'c|d']

        var = 'a|b|c|d'
        assert var.split('|', 3) == ['a', 'b', 'c', 'd']

        var = 'a|b|c|d'
        assert var.split('|', 4) == ['a', 'b', 'c', 'd']

        var = 'a|b|c|d'
        assert var.split('|', sys.maxint-2) == ['a', 'b', 'c', 'd']

        var = 'a|b|c|d'
        assert var.split('|', 0) == ['a|b|c|d']

        var = 'a||b||c||d'
        assert var.split('|', 2) == ['a', '', 'b||c||d']

        var = 'endcase |'
        assert var.split('|') == ['endcase ', '']

        var = '| startcase'
        assert var.split('|') == ['', ' startcase']

        var = '|bothcase|'
        assert var.split('|') == ['', 'bothcase', '']

        var = 'a\x00\x00b\x00c\x00d'
        assert var.split('\x00', 2) == ['a', '', 'b\x00c\x00d']

        var = ('a|'*20)[:-1]
        assert var.split('|') == ['a']*20

        var = ('a|'*20)[:-1]
        assert var.split('|', 15) == ['a']*15 +['a|a|a|a|a']

        # by string
        var = 'a//b//c//d'
        assert var.split('//') == ['a', 'b', 'c', 'd']

        var = 'a//b//c//d'
        assert var.split('//', 1) == ['a', 'b//c//d']

        var = 'a//b//c//d'
        assert var.split('//', 2) == ['a', 'b', 'c//d']

        var = 'a//b//c//d'
        assert var.split('//', 3) == ['a', 'b', 'c', 'd']

        var = 'a//b//c//d'
        assert var.split('//', 4) == ['a', 'b', 'c', 'd']

        var = 'a//b//c//d'
        assert var.split('//', sys.maxint-10) == ['a', 'b', 'c', 'd']

        var = 'a//b//c//d'
        assert var.split('//', 0) == ['a//b//c//d']

        var = 'a////b////c////d'
        assert var.split('//', 2) == ['a', '', 'b////c////d']

        var = 'endcase test'
        assert var.split('test') == ['endcase ', '']

        var = 'test begincase'
        assert var.split('test') == ['', ' begincase']

        var = 'test bothcase test'
        assert var.split('test') == ['', ' bothcase ', '']

        var = 'abbbc'
        assert var.split('bb') == ['a', 'bc']

        var = 'aaa'
        assert var.split('aaa') == ['', '']

        var = 'aaa'
        assert var.split('aaa', 0) == ['aaa']

        var = 'abbaab'
        assert var.split('ba') == ['ab', 'ab']

        var = 'aaaa'
        assert var.split('aab') == ['aaaa']

        var = ''
        assert var.split('aaa') == ['']

        var = 'aa'
        assert var.split('aaa') == ['aa']

        var = 'Abbobbbobb'
        assert var.split('bbobb') == ['A', 'bobb']

        var = 'AbbobbBbbobb'
        assert var.split('bbobb') == ['A', 'B', '']

        var = ('aBLAH'*20)[:-4]
        assert var.split('BLAH') == ['a']*20

        var = ('aBLAH'*20)[:-4]
        assert var.split('BLAH', 19) == ['a']*20

        var = ('aBLAH'*20)[:-4]
        assert var.split('BLAH', 18) == ['a']*18 + ['aBLAHa']

        ## mixed use of str and unicode
        #self.checkequal([u'a', u'b', u'c d'], 'a b c d', 'split', u' ', 2)

        ## argument type
        #self.checkraises(TypeError, 'hello', 'split', 42, 42, 42)

        ## null case
        #self.checkraises(ValueError, 'hello', 'split', '')
        #self.checkraises(ValueError, 'hello', 'split', '', 0)

    def test_rsplit(self):
        var = 'this is the rsplit function'
        assert var.rsplit() == ['this', 'is', 'the', 'rsplit', 'function']

        # by whitespace
        var = 'a b c d '
        assert var.rsplit() == ['a', 'b', 'c', 'd']

        var = 'a b c d'
        assert var.rsplit(None, 1) == ['a b c', 'd']

        var = 'a b c d'
        assert var.rsplit(None, 2) == ['a b', 'c', 'd']

        var = 'a b c d'
        assert var.rsplit(None, 3) == ['a', 'b', 'c', 'd']

        var = 'a b c d'
        assert var.rsplit(None, 4) == ['a', 'b', 'c', 'd']

        var = 'a b c d'
        assert var.rsplit(None, sys.maxint-20) == ['a', 'b', 'c', 'd']

        var = 'a b c d'
        assert var.rsplit(None, 0) == ['a b c d']

        var = 'a b c d  '
        assert var.rsplit(None, 0) == ['a b c d']

        var = 'a  b  c  d'
        assert var.rsplit(None, 2) == ['a  b', 'c', 'd']

        var = '         '
        assert var.rsplit() == []

        var = '  a    '
        assert var.rsplit() == ['a']

        var = '  a    b   '
        assert var.rsplit() == ['a', 'b']

        var = '  a    b   '
        assert var.rsplit(None, 1) == ['  a', 'b']

        var = '  a    b   c   '
        assert var.rsplit(None, 1) == ['  a    b', 'c']

        var = '  a    b   c   '
        assert var.rsplit(None, 2) == ['  a', 'b', 'c']

        var = '\n\ta \t\r b \v '
        assert var.rsplit(None, 88) == ['a', 'b']


        aaa = ' a '*20
        assert aaa.rsplit() == ['a']*20
        assert aaa.rsplit(None, 1) == [aaa[:-4]] + ['a']
        assert aaa.rsplit(None, 18) == [' a  a'] + ['a']*18

        # by a char
        var = 'a|b|c|d'
        assert var.rsplit('|') == ['a', 'b', 'c', 'd']

        var = 'a|b|c|d'
        assert var.rsplit('|', 1) == ['a|b|c', 'd']

        var = 'a|b|c|d'
        assert var.rsplit('|', 2) == ['a|b', 'c', 'd']

        var = 'a|b|c|d'
        assert var.rsplit('|', 3) == ['a', 'b', 'c', 'd']

        var = 'a|b|c|d'
        assert var.rsplit('|', 4) == ['a', 'b', 'c', 'd']

        var = 'a|b|c|d'
        assert var.rsplit('|', sys.maxint-100) == ['a', 'b', 'c', 'd']

        var = 'a|b|c|d'
        assert var.rsplit('|', 0) == ['a|b|c|d']

        var = 'a||b||c||d'
        assert var.rsplit('|', 2) == ['a||b||c', '', 'd']

        var = '| begincase'
        assert var.rsplit('|') == ['', ' begincase']

        var = 'endcase |'
        assert var.rsplit('|') == ['endcase ', '']

        var = '|bothcase|'
        assert var.rsplit('|') == ['', 'bothcase', '']

        var = 'a\x00\x00b\x00c\x00d'
        assert var.rsplit('\x00', 2) == ['a\x00\x00b', 'c', 'd']

        var = ('a|'*20)[:-1]
        assert var.rsplit('|') == ['a']*20

        var = ('a|'*20)[:-1]
        assert var.rsplit('|', 15) == ['a|a|a|a|a']+['a']*15

        # by string
        var = 'a//b//c//d'
        assert var.rsplit('//') == ['a', 'b', 'c', 'd']
        assert var.rsplit('//', 1) == ['a//b//c', 'd']
        assert var.rsplit('//', 2) == ['a//b', 'c', 'd']
        assert var.rsplit('//', 3) == ['a', 'b', 'c', 'd']
        assert var.rsplit('//', 4) == ['a', 'b', 'c', 'd']
        assert var.rsplit('//', sys.maxint-5) == ['a', 'b', 'c', 'd']
        assert var.rsplit('//', 0) == ['a//b//c//d']

        var = 'a////b////c////d'
        assert var.rsplit('//', 2) == ['a////b////c', '', 'd']

        var = 'test begincase'
        assert var.rsplit('test') == ['', ' begincase']

        var = 'endcase test'
        assert var.rsplit('test') == ['endcase ', '']

        var = 'test bothcase test'
        assert var.rsplit('test') == ['', ' bothcase ', '']

        var = 'abbbc'
        assert var.rsplit('bb') == ['ab', 'c']

        var = 'aaa'
        assert var.rsplit('aaa') == ['', '']

        var = 'aaa'
        assert var.rsplit('aaa', 0) == ['aaa']

        var = 'abbaab'
        assert var.rsplit('ba') == ['ab', 'ab']

        var = 'aaaa'
        assert var.rsplit('aab') == ['aaaa']

        var = ''
        assert var.rsplit('aaa') == ['']

        var = 'aa'
        assert var.rsplit('aaa') == ['aa']

        var = 'bbobbbobbA'
        assert var.rsplit('bbobb') == ['bbob', 'A']

        var = 'bbobbBbbobbA'
        assert var.rsplit('bbobb') == ['', 'B', 'A']

        var = ('aBLAH'*20)[:-4]
        assert var.rsplit('BLAH') == ['a']*20

        var = ('aBLAH'*20)[:-4]
        assert var.rsplit('BLAH', 19) == ['a']*20

        var = ('aBLAH'*20)[:-4]
        assert var.rsplit('BLAH', 18) == ['aBLAHa'] + ['a']*18

        ## mixed use of str and unicode
        #self.checkequal([u'a b', u'c', u'd'], 'a b c d', 'rsplit', u' ', 2)

        ## argument type
        #self.checkraises(TypeError, 'hello', 'rsplit', 42, 42, 42)

        ## null case
        #self.checkraises(ValueError, 'hello', 'rsplit', '')
        #self.checkraises(ValueError, 'hello', 'rsplit', '', 0)

    def test_strip(self):
        var = '   hello   '
        assert var.strip() == 'hello'

        var = '   hello   '
        assert var.lstrip() == 'hello   '

        var = '   hello   '
        assert var.rstrip() == '   hello'

        var = 'hello'
        assert var.strip() == 'hello'

        # strip/lstrip/rstrip with None arg
        var = '   hello   '
        assert var.strip(None) == 'hello'

        var = '   hello   '
        assert var.lstrip(None) == 'hello   '

        var = '   hello   '
        assert var.rstrip(None) == '   hello'

        var = 'hello'
        assert var.strip(None) == 'hello'

        ## strip/lstrip/rstrip with str arg
        var = 'xyzzyhelloxyzzy'
        assert var.strip('xyz') == 'hello' #self.checkequal('hello', 'xyzzyhelloxyzzy', 'strip', 'xyz')
        var = 'xyzzyhelloxyzzy'
        assert var.lstrip('xyz') == 'helloxyzzy' #self.checkequal('helloxyzzy', 'xyzzyhelloxyzzy', 'lstrip', 'xyz')
        var = 'xyzzyhelloxyzzy'
        assert var.rstrip('xyz') == 'xyzzyhello' #self.checkequal('xyzzyhello', 'xyzzyhelloxyzzy', 'rstrip', 'xyz')
        var = 'hello'
        assert var.strip('xyz') == 'hello' #self.checkequal('hello', 'hello', 'strip', 'xyz')

        ## strip/lstrip/rstrip with unicode arg
        #if test_support.have_unicode:
            #self.checkequal(unicode('hello', 'ascii'), 'xyzzyhelloxyzzy',
                 #'strip', unicode('xyz', 'ascii'))
            #self.checkequal(unicode('helloxyzzy', 'ascii'), 'xyzzyhelloxyzzy',
                 #'lstrip', unicode('xyz', 'ascii'))
            #self.checkequal(unicode('xyzzyhello', 'ascii'), 'xyzzyhelloxyzzy',
                 #'rstrip', unicode('xyz', 'ascii'))
            ## XXX
            ##self.checkequal(unicode('hello', 'ascii'), 'hello',
            ##     'strip', unicode('xyz', 'ascii'))

        #self.checkraises(TypeError, 'hello', 'strip', 42, 42)
        #self.checkraises(TypeError, 'hello', 'lstrip', 42, 42)
        #self.checkraises(TypeError, 'hello', 'rstrip', 42, 42)

    def test_ljust(self):
        var = 'abc'
        assert var.ljust(10) == 'abc       ' #self.checkequal('abc       ', 'abc', 'ljust', 10)
        var = 'abc'
        assert var.ljust(6) == 'abc   ' #self.checkequal('abc   ', 'abc', 'ljust', 6)
        var = 'abc'
        assert var.ljust(3) == 'abc' #self.checkequal('abc', 'abc', 'ljust', 3)
        var = 'abc'
        assert var.ljust(2) == 'abc' #self.checkequal('abc', 'abc', 'ljust', 2)
        assert var.ljust(10, '*') == 'abc*******'
        #self.checkraises(TypeError, 'abc', 'ljust')

    def test_rjust(self):
        var = 'abc'
        assert var.rjust(10) == '       abc' #self.checkequal('       abc', 'abc', 'rjust', 10)
        var = 'abc'
        assert var.rjust(6) == '   abc' #self.checkequal('   abc', 'abc', 'rjust', 6)
        var = 'abc'
        assert var.rjust(3) == 'abc' #self.checkequal('abc', 'abc', 'rjust', 3)
        var = 'abc'
        assert var.rjust(2) == 'abc' #self.checkequal('abc', 'abc', 'rjust', 2)
        assert var.rjust(10, '*') == '*******abc'
        #self.checkraises(TypeError, 'abc', 'rjust')

    def test_center(self):
        var = 'abc'
        assert var.center(10) == '   abc    ' #self.checkequal('   abc    ', 'abc', 'center', 10)
        var = 'abc'
        assert var.center(6) == ' abc  ' #self.checkequal(' abc  ', 'abc', 'center', 6)
        var = 'abc'
        assert var.center(3) == 'abc' #self.checkequal('abc', 'abc', 'center', 3)
        var = 'abc'
        assert var.center(2) == 'abc' #self.checkequal('abc', 'abc', 'center', 2)
        assert var.center(10, '*') == '***abc****'
        #self.checkraises(TypeError, 'abc', 'center')

    def test_swapcase(self):
        var = 'HeLLo cOmpUteRs'
        assert var.swapcase() == 'hEllO CoMPuTErS'

        #self.checkraises(TypeError, 'hello', 'swapcase', 42)

    def test_replace(self):
        # Operations on the empty string
        var = ""
        assert var.replace("", "") == "" #self.checkequal("", "", 'replace', "", "")
        var = ""
        assert var.replace("", "A") == "A" #self.checkequal("A", "", 'replace', "", "A")
        var = ""
        assert var.replace("A", "") == "" #self.checkequal("", "", 'replace', "A", "")
        var = ""
        assert var.replace("A", "A") == "" #self.checkequal("", "", 'replace', "A", "A")
        var = ""
        assert var.replace("", "", 100) == "" #self.checkequal("", "", 'replace', "", "", 100)
        var = ""
        assert var.replace("", "", sys.maxint) == "" #self.checkequal("", "", 'replace', "", "", sys.maxint)

        # interleave (from=="", 'to' gets inserted everywhere)
        var = 'A'
        assert var.replace('', '') == 'A'
        assert var.replace('', '*') == '*A*'
        assert var.replace('', '*1') == '*1A*1'
        assert var.replace('', '*-#') == '*-#A*-#'

        var = 'AA'
        assert var.replace('', '*-') == '*-A*-A*-'
        assert var.replace('', '*-', -1) == '*-A*-A*-'
        assert var.replace('', '*-', sys.maxint) == '*-A*-A*-'
        assert var.replace('', '*-', 4) == '*-A*-A*-'
        assert var.replace('', '*-', 3) == '*-A*-A*-'
        assert var.replace('', '*-', 2) == '*-A*-A'
        assert var.replace('', '*-', 1) == '*-AA'
        assert var.replace('', '*-', 0) == 'AA'

        # single character deletion (from=="A", to=="")
        var = "A"
        assert var.replace("A", "") == "" #self.checkequal("", "A", 'replace', "A", "")
        var = "AAA"
        assert var.replace("A", "") == "" #self.checkequal("", "AAA", 'replace', "A", "")
        var = "AAA"
        assert var.replace("A", "", -1) == "" #self.checkequal("", "AAA", 'replace', "A", "", -1)
        var = "AAA"
        assert var.replace("A", "", sys.maxint) == "" #self.checkequal("", "AAA", 'replace', "A", "", sys.maxint)
        var = "AAA"
        assert var.replace("A", "", 4) == "" #self.checkequal("", "AAA", 'replace', "A", "", 4)
        var = "AAA"
        assert var.replace("A", "", 3) == "" #self.checkequal("", "AAA", 'replace', "A", "", 3)
        var = "AAA"
        assert var.replace("A", "", 2) == "A" #self.checkequal("A", "AAA", 'replace', "A", "", 2)
        var = "AAA"
        assert var.replace("A", "", 1) == "AA" #self.checkequal("AA", "AAA", 'replace', "A", "", 1)
        var = "AAA"
        assert var.replace("A", "", 0) == "AAA" #self.checkequal("AAA", "AAA", 'replace', "A", "", 0)
        var = "AAAAAAAAAA"
        assert var.replace("A", "") == "" #self.checkequal("", "AAAAAAAAAA", 'replace', "A", "")
        var = "ABACADA"
        assert var.replace("A", "") == "BCD" #self.checkequal("BCD", "ABACADA", 'replace', "A", "")
        var = "ABACADA"
        assert var.replace("A", "", -1) == "BCD" #self.checkequal("BCD", "ABACADA", 'replace', "A", "", -1)
        var = "ABACADA"
        assert var.replace("A", "", sys.maxint) == "BCD" #self.checkequal("BCD", "ABACADA", 'replace', "A", "", sys.maxint)
        var = "ABACADA"
        assert var.replace("A", "", 5) == "BCD" #self.checkequal("BCD", "ABACADA", 'replace', "A", "", 5)
        var = "ABACADA"
        assert var.replace("A", "", 4) == "BCD" #self.checkequal("BCD", "ABACADA", 'replace', "A", "", 4)
        var = "ABACADA"
        assert var.replace("A", "", 3) == "BCDA" #self.checkequal("BCDA", "ABACADA", 'replace', "A", "", 3)
        var = "ABACADA"
        assert var.replace("A", "", 2) == "BCADA" #self.checkequal("BCADA", "ABACADA", 'replace', "A", "", 2)
        var = "ABACADA"
        assert var.replace("A", "", 1) == "BACADA" #self.checkequal("BACADA", "ABACADA", 'replace', "A", "", 1)
        var = "ABACADA"
        assert var.replace("A", "", 0) == "ABACADA" #self.checkequal("ABACADA", "ABACADA", 'replace', "A", "", 0)
        var = "ABCAD"
        assert var.replace("A", "") == "BCD" #self.checkequal("BCD", "ABCAD", 'replace', "A", "")
        var = "ABCADAA"
        assert var.replace("A", "") == "BCD" #self.checkequal("BCD", "ABCADAA", 'replace', "A", "")
        var = "BCD"
        assert var.replace("A", "") == "BCD" #self.checkequal("BCD", "BCD", 'replace', "A", "")

        var = '*************'
        assert var.replace('A', '') == '*************'
        var = '^'+'A'*1000+'^'
        assert var.replace('A', '', 999) == '^A^'

        # substring deletion (from=="the", to=="")
        var = "the"
        assert var.replace("the", "") == "" #self.checkequal("", "the", 'replace', "the", "")
        var = "theater"
        assert var.replace("the", "") == "ater" #self.checkequal("ater", "theater", 'replace', "the", "")
        var = "thethe"
        assert var.replace("the", "") == "" #self.checkequal("", "thethe", 'replace', "the", "")
        var = "thethethethe"
        assert var.replace("the", "") == "" #self.checkequal("", "thethethethe", 'replace', "the", "")
        var = "theatheatheathea"
        assert var.replace("the", "") == "aaaa" #self.checkequal("aaaa", "theatheatheathea", 'replace', "the", "")
        var = "that"
        assert var.replace("the", "") == "that" #self.checkequal("that", "that", 'replace', "the", "")
        var = "thaet"
        assert var.replace("the", "") == "thaet" #self.checkequal("thaet", "thaet", 'replace', "the", "")

        var = 'here and there'
        assert var.replace('the', '') == 'here and re'

        var = 'here and there and there'
        assert var.replace('the', '', sys.maxint) == 'here and re and re'
        assert var.replace('the', '', -1) == 'here and re and re'
        assert var.replace('the', '', 3) == 'here and re and re'
        assert var.replace('the', '', 2) == 'here and re and re'
        assert var.replace('the', '', 1) == 'here and re and there'
        assert var.replace('the', '', 0) == 'here and there and there'
        assert var.replace('the', '') == 'here and re and re'

        var = 'abc'
        assert var.replace('the', '') == 'abc'
        var = 'abcdefg'
        assert var.replace('the', '') == 'abcdefg'

        # substring deletion (from=="bob", to=="")
        var = "bbobob"
        assert var.replace("bob", "") == "bob" #self.checkequal("bob", "bbobob", 'replace', "bob", "")
        var = "bbobobXbbobob"
        assert var.replace("bob", "") == "bobXbob" #self.checkequal("bobXbob", "bbobobXbbobob", 'replace', "bob", "")
        var = "aaaaaaabob"
        assert var.replace("bob", "") == "aaaaaaa" #self.checkequal("aaaaaaa", "aaaaaaabob", 'replace', "bob", "")
        var = "aaaaaaa"
        assert var.replace("bob", "") == "aaaaaaa" #self.checkequal("aaaaaaa", "aaaaaaa", 'replace', "bob", "")

        # single character replace in place (len(from)==len(to)==1)
        var = 'Who goes there?'
        assert var.replace('o', 'o') == 'Who goes there?'
        assert var.replace('o', 'O') == 'WhO gOes there?'
        assert var.replace('o', 'O', sys.maxint) == 'WhO gOes there?'
        assert var.replace('o', 'O', -1) == 'WhO gOes there?'
        assert var.replace('o', 'O', 3) == 'WhO gOes there?'
        assert var.replace('o', 'O', 2) == 'WhO gOes there?'
        assert var.replace('o', 'O', 1) == 'WhO goes there?'
        assert var.replace('o', 'O', 0) == 'Who goes there?'

        assert var.replace('a', 'q') == 'Who goes there?'
        assert var.replace('W', 'w') == 'who goes there?'

        var = 'WWho goes there?WW'
        assert var.replace('W', 'w') == 'wwho goes there?ww'
        var = 'Who goes there?'
        assert var.replace('?', '!') == 'Who goes there!'
        var = 'Who goes there??'
        assert var.replace('?', '!') == 'Who goes there!!'
        var = 'Who goes there?'
        assert var.replace('.', '!') == 'Who goes there?'

        # substring replace in place (len(from)==len(to) > 1)
        var = 'This is a tissue'
        assert var.replace('is', '**') == 'Th** ** a t**sue'
        assert var.replace('is', '**', sys.maxint) == 'Th** ** a t**sue'
        assert var.replace('is', '**', -1) == 'Th** ** a t**sue'
        assert var.replace('is', '**', 4) == 'Th** ** a t**sue'
        assert var.replace('is', '**', 3) == 'Th** ** a t**sue'
        assert var.replace('is', '**', 2) == 'Th** ** a tissue'
        assert var.replace('is', '**', 1) == 'Th** is a tissue'
        assert var.replace('is', '**', 0) == 'This is a tissue'

        var = "bobob"
        assert var.replace("bob", "cob") == "cobob"
        var = "bobobXbobobob"
        assert var.replace("bob", "cob") == "cobobXcobocob"
        var = "bobob"
        assert var.replace("bot", "bot") == "bobob"

        # replace single character (len(from)==1, len(to)>1)
        var = "Reykjavik"
        assert var.replace("k", "KK") == "ReyKKjaviKK" #self.checkequal("ReyKKjaviKK", "Reykjavik", 'replace', "k", "KK")
        var = "Reykjavik"
        assert var.replace("k", "KK", -1) == "ReyKKjaviKK" #self.checkequal("ReyKKjaviKK", "Reykjavik", 'replace', "k", "KK", -1)
        var = "Reykjavik"
        assert var.replace("k", "KK", sys.maxint) == "ReyKKjaviKK" #self.checkequal("ReyKKjaviKK", "Reykjavik", 'replace', "k", "KK", sys.maxint)
        var = "Reykjavik"
        assert var.replace("k", "KK", 2) == "ReyKKjaviKK" #self.checkequal("ReyKKjaviKK", "Reykjavik", 'replace', "k", "KK", 2)
        var = "Reykjavik"
        assert var.replace("k", "KK", 1) == "ReyKKjavik" #self.checkequal("ReyKKjavik", "Reykjavik", 'replace', "k", "KK", 1)
        var = "Reykjavik"
        assert var.replace("k", "KK", 0) == "Reykjavik" #self.checkequal("Reykjavik", "Reykjavik", 'replace', "k", "KK", 0)
        #EQ("A----B----C----", "A.B.C.", "replace", ".", "----")

        var = "Reykjavik"
        assert var.replace("q", "KK") == "Reykjavik" #self.checkequal("Reykjavik", "Reykjavik", 'replace', "q", "KK")

        # replace substring (len(from)>1, len(to)!=len(from))
        var = 'spam, spam, eggs and spam'
        assert var.replace('spam', 'ham') == 'ham, ham, eggs and ham'
        assert var.replace('spam', 'ham', sys.maxint) == 'ham, ham, eggs and ham'
        assert var.replace('spam', 'ham', -1) == 'ham, ham, eggs and ham'
        assert var.replace('spam', 'ham', 4) == 'ham, ham, eggs and ham'
        assert var.replace('spam', 'ham', 3) == 'ham, ham, eggs and ham'
        assert var.replace('spam', 'ham', 2) == 'ham, ham, eggs and spam'
        assert var.replace('spam', 'ham', 1) == 'ham, spam, eggs and spam'
        assert var.replace('spam', 'ham', 0) == 'spam, spam, eggs and spam'

        var = "bobobob"
        assert var.replace("bobob", "bob") == "bobob" #self.checkequal("bobob", "bobobob", 'replace', "bobob", "bob")
        var = "bobobobXbobobob"
        assert var.replace("bobob", "bob") == "bobobXbobob" #self.checkequal("bobobXbobob", "bobobobXbobobob", 'replace', "bobob", "bob")
        var = "BOBOBOB"
        assert var.replace("bob", "bobby") == "BOBOBOB" #self.checkequal("BOBOBOB", "BOBOBOB", 'replace', "bob", "bobby")

        #with test_support.check_py3k_warnings():
            #ba = buffer('a')
            #bb = buffer('b')
        #var = 'abc'
        #assert var.replace(ba, bb) == 'bbc'
        #var = 'abc'
        #assert var.replace(bb, ba) == 'aac'

        #
        var = 'one!two!three!'
        assert var.replace('!', '') == 'onetwothree'
        assert var.replace('!', '@', 1) == 'one@two!three!'
        assert var.replace('!', '@', 2) == 'one@two@three!'
        assert var.replace('!', '@', 3) == 'one@two@three@'
        assert var.replace('!', '@', 4) == 'one@two@three@'
        assert var.replace('!', '@', 0) == 'one!two!three!'
        assert var.replace('!', '@') == 'one@two@three@'
        assert var.replace('x', '@') == 'one!two!three!'
        assert var.replace('x', '@', 2) == 'one!two!three!'

        var = 'abc'
        assert var.replace('', '-') == '-a-b-c-' #self.checkequal('-a-b-c-', 'abc', 'replace', '', '-')
        var = 'abc'
        assert var.replace('', '-', 3) == '-a-b-c' #self.checkequal('-a-b-c', 'abc', 'replace', '', '-', 3)
        var = 'abc'
        assert var.replace('', '-', 0) == 'abc' #self.checkequal('abc', 'abc', 'replace', '', '-', 0)
        var = ''
        assert var.replace('', '') == '' #self.checkequal('', '', 'replace', '', '')
        var = 'abc'
        assert var.replace('ab', '--', 0) == 'abc' #self.checkequal('abc', 'abc', 'replace', 'ab', '--', 0)
        var = 'abc'
        assert var.replace('xy', '--') == 'abc' #self.checkequal('abc', 'abc', 'replace', 'xy', '--')
        # Next three for SF bug 422088: [OSF1 alpha] string.replace(); died with
        # MemoryError due to empty result (platform malloc issue when requesting
        # 0 bytes).
        var = '123'
        assert var.replace('123', '') == '' #self.checkequal('', '123', 'replace', '123', '')
        var = '123123'
        assert var.replace('123', '') == '' #self.checkequal('', '123123', 'replace', '123', '')
        var = '123x123'
        assert var.replace('123', '') == 'x' #self.checkequal('x', '123x123', 'replace', '123', '')

        try:
            var = 'hello'
            var.replace()
            assert 'Should raise TypeError: replace() takes at least 2 arguments (0 given)' == False
        except TypeError:
            assert True

        try:
            var = 'hello'
            var.replace(42)
            assert 'Should raise TypeError: replace() takes at least 2 arguments (1 given)' == False
        except TypeError:
            assert True

        try:
            var = 'hello'
            var.replace('h', 42)
            assert 'Should raise TypeError: expected a character buffer object' == False
        except TypeError:
            assert True

    def test_replace_overflow(self):
        ## Check for overflow checking on 32 bit machines
        #if sys.maxint != 2147483647 or struct.calcsize("P") > 4:
            #return
        #A2_16 = "A" * (2**16)
        #self.checkraises(OverflowError, A2_16, "replace", "", A2_16)
        #self.checkraises(OverflowError, A2_16, "replace", "A", A2_16)
        #self.checkraises(OverflowError, A2_16, "replace", "AA", A2_16+A2_16)
        pass

    def test_zfill(self):
        var = '123'
        assert var.zfill(2) == '123' #self.checkequal('123', '123', 'zfill', 2)
        var = '123'
        assert var.zfill(3) == '123' #self.checkequal('123', '123', 'zfill', 3)
        var = '123'
        assert var.zfill(4) == '0123' #self.checkequal('0123', '123', 'zfill', 4)

        var = '+123'
        assert var.zfill(3) == '+123'

        var = '+123'
        assert var.zfill(4) == '+123'

        var = '+123'
        assert var.zfill(5) == '+0123'

        var = '-123'
        assert var.zfill(3) == '-123'

        var = '-123'
        assert var.zfill(4) == '-123'

        var = '-123'
        assert var.zfill(5) == '-0123'

        var = ''
        assert var.zfill(3) == '000' #self.checkequal('000', '', 'zfill', 3)
        var = '34'
        assert var.zfill(1) == '34' #self.checkequal('34', '34', 'zfill', 1)
        var = '34'
        assert var.zfill(4) == '0034' #self.checkequal('0034', '34', 'zfill', 4)

        #self.checkraises(TypeError, '123', 'zfill')


class MixinStrUnicodeUserStringTest(object):
    ## additional tests that only work for
    ## stringlike objects, i.e. str, unicode, UserString
    ## (but not the string module)

    def test_islower(self):
        var = ''
        assert var.islower() == False #self.checkequal(False, '', 'islower')
        var = 'a'
        assert var.islower() == True #self.checkequal(True, 'a', 'islower')
        var = 'A'
        assert var.islower() == False #self.checkequal(False, 'A', 'islower')
        var = '\n'
        assert var.islower() == False #self.checkequal(False, '\n', 'islower')
        var = 'abc'
        assert var.islower() == True #self.checkequal(True, 'abc', 'islower')
        var = 'aBc'
        assert var.islower() == False #self.checkequal(False, 'aBc', 'islower')
        var = 'abc\n'
        assert var.islower() == True #self.checkequal(True, 'abc\n', 'islower')
        #self.checkraises(TypeError, 'abc', 'islower', 42)

    def test_isupper(self):
        var = ''
        assert var.isupper() == False #self.checkequal(False, '', 'isupper')
        var = 'a'
        assert var.isupper() == False #self.checkequal(False, 'a', 'isupper')
        var = 'A'
        assert var.isupper() == True #self.checkequal(True, 'A', 'isupper')
        #self.checkequal(False, '\n', 'isupper')
        var = 'ABC'
        assert var.isupper() == True #self.checkequal(True, 'ABC', 'isupper')
        var = 'AbC'
        assert var.isupper() == False #self.checkequal(False, 'AbC', 'isupper')
        #self.checkequal(True, 'ABC\n', 'isupper')
        #self.checkraises(TypeError, 'abc', 'isupper', 42)

    def test_istitle(self):
        var = ''
        assert var.istitle() == False #self.checkequal(False, '', 'istitle')
        var = 'a'
        assert var.istitle() == False #self.checkequal(False, 'a', 'istitle')
        var = 'A'
        assert var.istitle() == True #self.checkequal(True, 'A', 'istitle')
        var = '\n'
        assert var.istitle() == False
        var = 'A Titlecased Line'
        assert var.istitle() == True
        var = 'A\nTitlecased Line'
        assert var.istitle() == True
        var = 'A Titlecased, Line'
        assert var.istitle() == True
        var = 'Not a capitalized String'
        assert var.istitle() == False
        var = 'Not\ta Titlecase String'
        assert var.istitle() == False
        var = 'Not--a Titlecase String'
        assert var.istitle() == False

        var = 'NOT'
        assert var.istitle() == False #self.checkequal(False, 'NOT', 'istitle')
        #self.checkraises(TypeError, 'abc', 'istitle', 42)

    def test_isspace(self):
        var = ''
        assert var.isspace() == False #self.checkequal(False, '', 'isspace')
        var = 'a'
        assert var.isspace() == False #self.checkequal(False, 'a', 'isspace')
        #self.checkequal(True, ' ', 'isspace')
        #self.checkequal(True, '\t', 'isspace')
        #self.checkequal(True, '\r', 'isspace')
        #self.checkequal(True, '\n', 'isspace')
        #self.checkequal(True, ' \t\r\n', 'isspace')
        #self.checkequal(False, ' \t\r\na', 'isspace')
        #self.checkraises(TypeError, 'abc', 'isspace', 42)

    def test_isalpha(self):
        var = ''
        assert var.isalpha() == False #self.checkequal(False, '', 'isalpha')
        var = 'a'
        assert var.isalpha() == True #self.checkequal(True, 'a', 'isalpha')
        var = 'A'
        assert var.isalpha() == True #self.checkequal(True, 'A', 'isalpha')
        #self.checkequal(False, '\n', 'isalpha')
        var = 'abc'
        assert var.isalpha() == True #self.checkequal(True, 'abc', 'isalpha')
        var = 'aBc123'
        assert var.isalpha() == False #self.checkequal(False, 'aBc123', 'isalpha')
        #self.checkequal(False, 'abc\n', 'isalpha')
        #self.checkraises(TypeError, 'abc', 'isalpha', 42)

    def test_isalnum(self):
        var = ''
        assert var.isalnum() == False #self.checkequal(False, '', 'isalnum')
        var = 'a'
        assert var.isalnum() == True #self.checkequal(True, 'a', 'isalnum')
        var = 'A'
        assert var.isalnum() == True #self.checkequal(True, 'A', 'isalnum')
        #self.checkequal(False, '\n', 'isalnum')
        var = '123abc456'
        assert var.isalnum() == True #self.checkequal(True, '123abc456', 'isalnum')
        var = 'a1b3c'
        assert var.isalnum() == True #self.checkequal(True, 'a1b3c', 'isalnum')
        #self.checkequal(False, 'aBc000 ', 'isalnum')
        #self.checkequal(False, 'abc\n', 'isalnum')
        #self.checkraises(TypeError, 'abc', 'isalnum', 42)

    def test_isdigit(self):
        var = ''
        assert var.isdigit() == False #self.checkequal(False, '', 'isdigit')
        var = 'a'
        assert var.isdigit() == False #self.checkequal(False, 'a', 'isdigit')
        var = '0'
        assert var.isdigit() == True #self.checkequal(True, '0', 'isdigit')
        var = '0123456789'
        assert var.isdigit() == True #self.checkequal(True, '0123456789', 'isdigit')
        var = '0123456789a'
        assert var.isdigit() == False #self.checkequal(False, '0123456789a', 'isdigit')

        #self.checkraises(TypeError, 'abc', 'isdigit', 42)

    def test_title(self):
        var = ' hello '
        assert var.title() == ' Hello '
        var = 'hello '
        assert var.title() == 'Hello '
        var = 'Hello '
        assert var.title() == 'Hello '
        var = 'fOrMaT thIs aS titLe String'
        assert var.title() == 'Format This As Title String'
        var = 'fOrMaT,thIs-aS*titLe;String'
        assert var.title() == 'Format,This-As*Title;String'
        var = 'getInt'
        assert var.title() == 'Getint'

        #self.checkraises(TypeError, 'hello', 'title', 42)

    def test_splitlines(self):
        var = "abc\ndef\n\rghi"
        assert var.splitlines() == ['abc', 'def', '', 'ghi']

        var = 'abc\ndef\n\r\nghi'
        assert var.splitlines() == ['abc', 'def', '', 'ghi']

        var = 'abc\ndef\r\nghi'
        assert var.splitlines() == ['abc', 'def', 'ghi']

        var = 'abc\ndef\r\nghi\n'
        assert var.splitlines() == ['abc', 'def', 'ghi']

        var = 'abc\ndef\r\nghi\n\r'
        assert var.splitlines() == ['abc', 'def', 'ghi', '']

        var = '\nabc\ndef\r\nghi\n\r'
        assert var.splitlines() == ['', 'abc', 'def', 'ghi', '']

        var = '\nabc\ndef\r\nghi\n\r'
        assert var.splitlines(1) == ['\n', 'abc\n', 'def\r\n', 'ghi\n', '\r']

        #self.checkraises(TypeError, 'abc', 'splitlines', 42, 42)

    def test_startswith(self):
        var = 'hello'
        assert var.startswith('he') == True #self.checkequal(True, 'hello', 'startswith', 'he')
        var = 'hello'
        assert var.startswith('hello') == True #self.checkequal(True, 'hello', 'startswith', 'hello')
        var = 'hello'
        assert var.startswith('hello world') == False #self.checkequal(False, 'hello', 'startswith', 'hello world')
        var = 'hello'
        assert var.startswith('') == True #self.checkequal(True, 'hello', 'startswith', '')
        var = 'hello'
        assert var.startswith('ello') == False #self.checkequal(False, 'hello', 'startswith', 'ello')
        var = 'hello'
        assert var.startswith('ello', 1) == True #self.checkequal(True, 'hello', 'startswith', 'ello', 1)
        var = 'hello'
        assert var.startswith('o', 4) == True #self.checkequal(True, 'hello', 'startswith', 'o', 4)
        var = 'hello'
        assert var.startswith('o', 5) == False #self.checkequal(False, 'hello', 'startswith', 'o', 5)
        var = 'hello'
        assert var.startswith('', 5) == True #self.checkequal(True, 'hello', 'startswith', '', 5)
        var = 'hello'
        assert var.startswith('lo', 6) == False #self.checkequal(False, 'hello', 'startswith', 'lo', 6)
        var = 'helloworld'
        assert var.startswith('lowo', 3) == True #self.checkequal(True, 'helloworld', 'startswith', 'lowo', 3)
        var = 'helloworld'
        assert var.startswith('lowo', 3, 7) == True #self.checkequal(True, 'helloworld', 'startswith', 'lowo', 3, 7)
        var = 'helloworld'
        assert var.startswith('lowo', 3, 6) == False #self.checkequal(False, 'helloworld', 'startswith', 'lowo', 3, 6)

        # test negative indices
        var = 'hello'
        assert var.startswith('he', 0, -1) == True #self.checkequal(True, 'hello', 'startswith', 'he', 0, -1)
        var = 'hello'
        assert var.startswith('he', -53, -1) == True #self.checkequal(True, 'hello', 'startswith', 'he', -53, -1)
        # TODO: this is not supported by Pyston yet
        #var = 'hello'
        #assert var.startswith('hello', 0, -1) == False
        var = 'hello'
        assert var.startswith('hello world', -1, -10) == False #self.checkequal(False, 'hello', 'startswith', 'hello world', -1, -10)
        var = 'hello'
        assert var.startswith('ello', -5) == False #self.checkequal(False, 'hello', 'startswith', 'ello', -5)
        var = 'hello'
        assert var.startswith('ello', -4) == True #self.checkequal(True, 'hello', 'startswith', 'ello', -4)
        var = 'hello'
        assert var.startswith('o', -2) == False #self.checkequal(False, 'hello', 'startswith', 'o', -2)
        var = 'hello'
        assert var.startswith('o', -1) == True #self.checkequal(True, 'hello', 'startswith', 'o', -1)
        var = 'hello'
        assert var.startswith('', -3, -3) == True #self.checkequal(True, 'hello', 'startswith', '', -3, -3)
        var = 'hello'
        assert var.startswith('lo', -9) == False #self.checkequal(False, 'hello', 'startswith', 'lo', -9)

        #self.checkraises(TypeError, 'hello', 'startswith')
        #self.checkraises(TypeError, 'hello', 'startswith', 42)

        # test tuple arguments
        #var = 'hello'
        #assert var.startswith(('he', 'ha')) == True
        #var = 'hello'
        #assert var.startswith(('lo', 'llo')) == False
        #var = 'hello'
        #assert var.startswith(('hellox', 'hello')) == True
        #var = 'hello'
        #assert var.startswith(()) == False
        #var = 'helloworld'
        #assert var.startswith(('hellowo', 'rld', 'lowo'), 3) == True
        #assert var.startswith(('hellowo', 'ello', 'rld'), 3) == False

        #var = 'hello'
        #assert var.startswith(('lo', 'he'), 0, -1) == True
        #assert var.startswith(('he', 'hel'), 0, 1) == False
        #assert var.startswith(('he', 'hel'), 0, 2) == True

        #self.checkraises(TypeError, 'hello', 'startswith', (42,))

    def test_endswith(self):
        var = 'hello'
        assert var.endswith('lo') == True #self.checkequal(True, 'hello', 'endswith', 'lo')
        assert var.endswith('he') == False #self.checkequal(False, 'hello', 'endswith', 'he')
        assert var.endswith('') == True #self.checkequal(True, 'hello', 'endswith', '')
        assert var.endswith('hello world') == False #self.checkequal(False, 'hello', 'endswith', 'hello world')

        var = 'helloworld'
        assert var.endswith('worl') == False #self.checkequal(False, 'helloworld', 'endswith', 'worl')
        assert var.endswith('worl', 3, 9) == True #self.checkequal(True, 'helloworld', 'endswith', 'worl', 3, 9)
        assert var.endswith('world', 3, 12) == True #self.checkequal(True, 'helloworld', 'endswith', 'world', 3, 12)
        assert var.endswith('lowo', 1, 7) == True #self.checkequal(True, 'helloworld', 'endswith', 'lowo', 1, 7)
        assert var.endswith('lowo', 2, 7) == True #self.checkequal(True, 'helloworld', 'endswith', 'lowo', 2, 7)
        assert var.endswith('lowo', 3, 7) == True #self.checkequal(True, 'helloworld', 'endswith', 'lowo', 3, 7)
        assert var.endswith('lowo', 4, 7) == False #self.checkequal(False, 'helloworld', 'endswith', 'lowo', 4, 7)
        assert var.endswith('lowo', 3, 8) == False #self.checkequal(False, 'helloworld', 'endswith', 'lowo', 3, 8)

        var = 'ab'
        assert var.endswith('ab', 0, 1) == False #self.checkequal(False, 'ab', 'endswith', 'ab', 0, 1)
        assert var.endswith('ab', 0, 0) == False #self.checkequal(False, 'ab', 'endswith', 'ab', 0, 0)

        # test negative indices
        var = 'hello'
        assert var.endswith('lo', -2) == True #self.checkequal(True, 'hello', 'endswith', 'lo', -2)
        assert var.endswith('he', -2) == False #self.checkequal(False, 'hello', 'endswith', 'he', -2)
        assert var.endswith('', -3, -3) == True #self.checkequal(True, 'hello', 'endswith', '', -3, -3)
        assert var.endswith('hello world', -10, -2) == False #self.checkequal(False, 'hello', 'endswith', 'hello world', -10, -2)

        var = 'helloworld'
        assert var.endswith('worl', -6) == False #self.checkequal(False, 'helloworld', 'endswith', 'worl', -6)
        # TODO: for some reason doesn't work in Pyston yet
        #assert var.endswith('worl', -5, -1) == True
        assert var.endswith('worl', -5, 9) == True #self.checkequal(True, 'helloworld', 'endswith', 'worl', -5, 9)
        assert var.endswith('world', -7, 12) == True #self.checkequal(True, 'helloworld', 'endswith', 'world', -7, 12)
        # TODO: for some reason doesn't work in Pyston yet
        #assert var.endswith('lowo', -99, -3) == True
        #assert var.endswith('lowo', -8, -3) == True
        #assert var.endswith('lowo', -7, -3) == True
        assert var.endswith('lowo', 3, -4) == False #self.checkequal(False, 'helloworld', 'endswith', 'lowo', 3, -4)
        assert var.endswith('lowo', -8, -2) == False #self.checkequal(False, 'helloworld', 'endswith', 'lowo', -8, -2)

        #self.checkraises(TypeError, 'hello', 'endswith')
        #self.checkraises(TypeError, 'hello', 'endswith', 42)

        # test tuple arguments
        # TODO: not implemented in Pyston yet
        #var = 'hello'
        #assert var.endswith(('he', 'ha')) == False
        #assert var.endswith(('lo', 'llo')) == True
        #assert var.endswith(('hellox', 'hello')) == True
        #assert var.endswith(()) == False

        #var = 'helloworld'
        #assert var.endswith(('hellowo', 'rld', 'lowo'), 3) == True
        #assert var.endswith(('hellowo', 'ello', 'rld'), 3, -1) == False

        #var = 'hello'
        #assert var.endswith(('hell', 'ell'), 0, -1) == True
        #assert var.endswith(('he', 'hel'), 0, 1) == False
        #assert var.endswith(('he', 'hell'), 0, 4) == True

        #self.checkraises(TypeError, 'hello', 'endswith', (42,))

    def test___contains__(self):
        var = ''
        assert var.__contains__('') == True
        var = 'abc'
        assert var.__contains__('') == True
        var = 'abc'
        assert var.__contains__('\0') == False
        var = '\0abc'
        assert var.__contains__('\0') == True
        var = 'abc\0'
        assert var.__contains__('\0') == True
        var = '\0abc'
        assert var.__contains__('a') == True
        var = 'asdf'
        assert var.__contains__('asdf') == True
        var = 'asd'
        assert var.__contains__('asdf') == False
        var = ''
        assert var.__contains__('asdf') == False

    def test_subscript(self):
        var = 'abc'
        assert var.__getitem__(0) == 'a'
        var = 'abc'
        assert var.__getitem__(-1) == 'c'
        #self.checkequal(u'a', 'abc', '__getitem__', 0L)
        var = 'abc'
        assert var.__getitem__(slice(0, 3)) == 'abc'
        var = 'abc'
        assert var.__getitem__(slice(0, 1000)) == 'abc'
        var = 'abc'
        assert var.__getitem__(slice(0, 1)) == 'a'
        var = 'abc'
        assert var.__getitem__(slice(0, 0)) == ''

        #self.checkraises(TypeError, 'abc', '__getitem__', 'def')

    def test_slice(self):
        var = 'abc'
        assert var.__getslice__(0, 1000) == 'abc'
        assert var.__getslice__(0, 3) == 'abc'
        assert var.__getslice__(0, 2) == 'ab'
        assert var.__getslice__(1, 3) == 'bc'
        assert var.__getslice__(1, 2) == 'b'
        assert var.__getslice__(2, 2) == ''
        assert var.__getslice__(1000, 1000) == ''
        assert var.__getslice__(2000, 1000) == ''
        assert var.__getslice__(2, 1) == ''

        #var = 'abc'
        #try:
            #var.__getslice__('def')
            #assert 'Show throw TypeError' == False
        #except TypeError:
            #assert True

    def test_extended_getslice(self):
        ## Test extended slicing by comparing with list slicing.
        ##s = string.ascii_letters + string.digits
        #ascii_letters = 'abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ'
        #digits = '0123456789'
        #s = ascii_letters + digits
        #indices = (0, None, 1, 3, 41, -1, -2, -37)
        #for start in indices:
            #for stop in indices:
                ## Skip step 0 (invalid)
                #for step in indices[1:]:
                    #L = list(s)[start:stop:step]
                    #assert s.__getitem__(slice(start, stop, step)) == "".join(L)
        pass

    def test_mul(self):
        var = 'abc'
        assert var.__mul__(-1) == ''
        assert var.__mul__(0) == ''
        assert var.__mul__(1) == 'abc'
        assert var.__mul__(3) == 'abcabcabc'

        #self.checkraises(TypeError, 'abc', '__mul__')
        #class Mul(object):
            #def mul(self, a, b):
                #return a * b
        #self.checkraises(TypeError, Mul(), 'mul', 'abc', '')
        ## XXX: on a 64-bit system, this doesn't raise an overflow error,
        ## but either raises a MemoryError, or succeeds (if you have 54TiB)
        ##self.checkraises(OverflowError, 10000*'abc', '__mul__', 2000000000)

    def test_join(self):
        # join now works with any sequence type
        # moved here
        var = ' '
        assert var.join(['a', 'b', 'c', 'd']) == 'a b c d'
        var = ''
        assert var.join(('a', 'b', 'c', 'd')) == 'abcd'
        var = ''
        assert var.join(('', 'b', '', 'd')) == 'bd' #self.checkequal('bd', '', 'join', ('', 'b', '', 'd'))
        var = ''
        assert var.join(('a', '', 'c', '')) == 'ac' #self.checkequal('ac', '', 'join', ('a', '', 'c', ''))
        #self.checkequal('w x y z', ' ', 'join', Sequence())
        var = 'a'
        assert var.join(('abc',)) == 'abc' #self.checkequal('abc', 'a', 'join', ('abc',))
        #var = 'a'
        #assert var.join(UserList(['z'])) == 'z' #self.checkequal('z', 'a', 'join', UserList(['z']))
        #if test_support.have_unicode:
            #self.checkequal(unicode('a.b.c'), unicode('.'), 'join', ['a', 'b', 'c'])
            #self.checkequal(unicode('a.b.c'), '.', 'join', [unicode('a'), 'b', 'c'])
            #self.checkequal(unicode('a.b.c'), '.', 'join', ['a', unicode('b'), 'c'])
            #self.checkequal(unicode('a.b.c'), '.', 'join', ['a', 'b', unicode('c')])
            #self.checkraises(TypeError, '.', 'join', ['a', unicode('b'), 3])
        #for i in [5, 25, 125]:
        #    var = '-'
        #    assert var.join(['a' * i] * i) == ((('a' * i) + '-') * i)[:-1]
        #    assert var.join(('a' * i,) * i) == ((('a' * i) + '-') * i)[:-1]

        #self.checkraises(TypeError, ' ', 'join', BadSeq1())
        #self.checkequal('a b c', ' ', 'join', BadSeq2())

        #self.checkraises(TypeError, ' ', 'join')
        #self.checkraises(TypeError, ' ', 'join', 7)
        #self.checkraises(TypeError, ' ', 'join', Sequence([7, 'hello', 123L]))
        #try:
            #def f():
                #yield 4 + ""
            #self.fixtype(' ').join(f())
        #except TypeError, e:
            #if '+' not in str(e):
                #self.fail('join() ate exception message')
        #else:
            #self.fail('exception not raised')

    def test_formatting(self):
        var = '+%s+'
        assert var.__mod__('hello') == '+hello+' #self.checkequal('+hello+', '+%s+', '__mod__', 'hello')
        #self.checkequal('+10+', '+%d+', '__mod__', 10)
        #self.checkequal('a', "%c", '__mod__', "a")
        #self.checkequal('a', "%c", '__mod__', "a")
        #self.checkequal('"', "%c", '__mod__', 34)
        #self.checkequal('$', "%c", '__mod__', 36)
        #self.checkequal('10', "%d", '__mod__', 10)
        #self.checkequal('\x7f', "%c", '__mod__', 0x7f)

        #for ordinal in (-100, 0x200000):
            ## unicode raises ValueError, str raises OverflowError
            #self.checkraises((ValueError, OverflowError), '%c', '__mod__', ordinal)

        #longvalue = sys.maxint + 10L
        #slongvalue = str(longvalue)
        #if slongvalue[-1] in ("L","l"): slongvalue = slongvalue[:-1]
        #self.checkequal(' 42', '%3ld', '__mod__', 42)
        #self.checkequal('42', '%d', '__mod__', 42L)
        #self.checkequal('42', '%d', '__mod__', 42.0)
        #self.checkequal(slongvalue, '%d', '__mod__', longvalue)
        #self.checkcall('%d', '__mod__', float(longvalue))
        #self.checkequal('0042.00', '%07.2f', '__mod__', 42)
        #self.checkequal('0042.00', '%07.2F', '__mod__', 42)

        #self.checkraises(TypeError, 'abc', '__mod__')
        #self.checkraises(TypeError, '%(foo)s', '__mod__', 42)
        #self.checkraises(TypeError, '%s%s', '__mod__', (42,))
        #self.checkraises(TypeError, '%c', '__mod__', (None,))
        #self.checkraises(ValueError, '%(foo', '__mod__', {})
        #self.checkraises(TypeError, '%(foo)s %(bar)s', '__mod__', ('foo', 42))
        #self.checkraises(TypeError, '%d', '__mod__', "42") # not numeric
        #self.checkraises(TypeError, '%d', '__mod__', (42+0j)) # no int/long conversion provided

        ## argument names with properly nested brackets are supported
        #self.checkequal('bar', '%((foo))s', '__mod__', {'(foo)': 'bar'})

        ## 100 is a magic number in PyUnicode_Format, this forces a resize
        #self.checkequal(103*'a'+'x', '%sx', '__mod__', 103*'a')

        #self.checkraises(TypeError, '%*s', '__mod__', ('foo', 'bar'))
        #self.checkraises(TypeError, '%10.*f', '__mod__', ('foo', 42.))
        #self.checkraises(ValueError, '%10', '__mod__', (42,))

        #width = int(_testcapi.PY_SSIZE_T_MAX + 1)
        #if width <= sys.maxint:
            #self.checkraises(OverflowError, '%*s', '__mod__', (width, ''))
        #prec = int(_testcapi.INT_MAX + 1)
        #if prec <= sys.maxint:
            #self.checkraises(OverflowError, '%.*f', '__mod__', (prec, 1. / 7))
        ## Issue 15989
        #width = int(1 << (_testcapi.PY_SSIZE_T_MAX.bit_length() + 1))
        #if width <= sys.maxint:
            #self.checkraises(OverflowError, '%*s', '__mod__', (width, ''))
        #prec = int(_testcapi.UINT_MAX + 1)
        #if prec <= sys.maxint:
            #self.checkraises(OverflowError, '%.*f', '__mod__', (prec, 1. / 7))

        #class X(object): pass
        #self.checkraises(TypeError, 'abc', '__mod__', X())
        #class X(Exception):
            #def __getitem__(self, k):
                #return k
        #self.checkequal('melon apple', '%(melon)s %(apple)s', '__mod__', X())

    def test_floatformatting(self):
        ## float formatting
        #for prec in xrange(100):
            #format = '%%.%if' % prec
            #value = 0.01
            #for x in xrange(60):
                #value = value * 3.14159265359 / 3.0 * 10.0
                #self.checkcall(format, "__mod__", value)
        pass

    def test_inplace_rewrites(self):
        ## Check that strings don't copy and modify cached single-character strings
        var = 'A'
        assert var.lower() == 'a' #self.checkequal('a', 'A', 'lower')
        var = 'A'
        assert var.isupper() == True #self.checkequal(True, 'A', 'isupper')
        var = 'a'
        assert var.upper() == 'A' #self.checkequal('A', 'a', 'upper')
        var = 'a'
        assert var.islower() == True #self.checkequal(True, 'a', 'islower')

        var = 'A'
        assert var.replace('A', 'a') == 'a' #self.checkequal('a', 'A', 'replace', 'A', 'a')
        var = 'A'
        assert var.isupper() == True #self.checkequal(True, 'A', 'isupper')

        var = 'a'
        assert var.capitalize() == 'A' #self.checkequal('A', 'a', 'capitalize')
        var = 'a'
        assert var.islower() == True #self.checkequal(True, 'a', 'islower')

        var = 'a'
        assert var.swapcase() == 'A' #self.checkequal('A', 'a', 'swapcase')
        var = 'a'
        assert var.islower() == True #self.checkequal(True, 'a', 'islower')

        var = 'a'
        assert var.title() == 'A' #self.checkequal('A', 'a', 'title')
        var = 'a'
        assert var.islower() == True #self.checkequal(True, 'a', 'islower')

    def test_partition(self):
        var = 'this is the partition method'
        assert var.partition('ti') == ('this is the par', 'ti', 'tion method')

        # from raymond's original specification
        S = 'http://www.python.org'
        assert S.partition('://') == ('http', '://', 'www.python.org')
        assert S.partition('?') == ('http://www.python.org', '', '')
        assert S.partition('http://') == ('', 'http://', 'www.python.org')
        assert S.partition('org') == ('http://www.python.', 'org', '')

        #self.checkraises(ValueError, S, 'partition', '')
        #self.checkraises(TypeError, S, 'partition', None)

        ## mixed use of str and unicode
        #self.assertEqual('a/b/c'.partition(u'/'), ('a', '/', 'b/c'))

    def test_rpartition(self):
        var = 'this is the rpartition method'
        assert var.rpartition('ti') == ('this is the rparti', 'ti', 'on method')

        # from raymond's original specification
        S = 'http://www.python.org'
        assert S.rpartition('://') == ('http', '://', 'www.python.org')
        assert S.rpartition('?') == ('', '', 'http://www.python.org')
        assert S.rpartition('http://') == ('', 'http://', 'www.python.org')
        assert S.rpartition('org') == ('http://www.python.', 'org', '')

        #self.checkraises(ValueError, S, 'rpartition', '')
        #self.checkraises(TypeError, S, 'rpartition', None)

        ## mixed use of str and unicode
        #self.assertEqual('a/b/c'.rpartition(u'/'), ('a/b', '/', 'c'))

    def test_none_arguments(self):
        ## issue 11828
        s = 'hello'
        assert s.find('l', None) == 2
        assert s.find('l', -2, None) == 3
        assert s.find('l', None, -2) == 2
        assert s.find('h', None, None) == 0

        assert s.rfind('l', None) == 3
        assert s.rfind('l', -2, None) == 3
        #assert s.rfind('l', None, -2) == 2
        assert s.rfind('h', None, None) == 0

        assert s.index('l', None) == 2
        assert s.index('l', -2, None) == 3
        assert s.index('l', None, -2) == 2
        assert s.index('h', None, None) == 0

        assert s.rindex('l', None) == 3
        assert s.rindex('l', -2, None) == 3
        #assert s.rindex('l', None, -2) == 2
        assert s.rindex('h', None, None) == 0

        assert s.count('l', None) == 2
        assert s.count('l', -2, None) == 1
        assert s.count('l', None, -2) == 1
        assert s.count('x', None, None) == 0

        assert s.endswith('o', None) == True
        assert s.endswith('lo', -2, None) == True
        assert s.endswith('l', None, -2) == True
        assert s.endswith('x', None, None) == False

        assert s.startswith('h', None) == True
        assert s.startswith('l', -2, None) == True
        assert s.startswith('h', None, -2) == True
        assert s.startswith('x', None, None) == False

    def test_find_etc_raise_correct_error_messages(self):
        ## issue 11828
        s = 'hello'
        x = 'x'
        #self.assertRaisesRegexp(TypeError, r'\bfind\b', s.find,
                                #x, None, None, None)
        #self.assertRaisesRegexp(TypeError, r'\brfind\b', s.rfind,
                                #x, None, None, None)
        #self.assertRaisesRegexp(TypeError, r'\bindex\b', s.index,
                                #x, None, None, None)
        #self.assertRaisesRegexp(TypeError, r'\brindex\b', s.rindex,
                                #x, None, None, None)
        #self.assertRaisesRegexp(TypeError, r'^count\(', s.count,
                                #x, None, None, None)
        #self.assertRaisesRegexp(TypeError, r'^startswith\(', s.startswith,
                                #x, None, None, None)
        #self.assertRaisesRegexp(TypeError, r'^endswith\(', s.endswith,
                                #x, None, None, None)

class MixinStrStringUserStringTest(object):
    ## Additional tests for 8bit strings, i.e. str, UserString and
    ## the string module

    #def test_maketrans(self):
        #self.assertEqual(
           #''.join(map(chr, xrange(256))).replace('abc', 'xyz'),
           #string.maketrans('abc', 'xyz')
        #)
        #self.assertRaises(ValueError, string.maketrans, 'abc', 'xyzw')

    def test_translate(self):
        #table = string.maketrans('abc', 'xyz')
        table = '\x00\x01\x02\x03\x04\x05\x06\x07\x08\t\n\x0b\x0c\r\x0e\x0f\x10\x11\x12\x13\x14\x15\x16\x17\x18\x19\x1a\x1b\x1c\x1d\x1e\x1f !"#$%&\'()*+,-./0123456789:;<=>?@ABCDEFGHIJKLMNOPQRSTUVWXYZ[\\]^_`xyzdefghijklmnopqrstuvwxyz{|}~\x7f\x80\x81\x82\x83\x84\x85\x86\x87\x88\x89\x8a\x8b\x8c\x8d\x8e\x8f\x90\x91\x92\x93\x94\x95\x96\x97\x98\x99\x9a\x9b\x9c\x9d\x9e\x9f\xa0\xa1\xa2\xa3\xa4\xa5\xa6\xa7\xa8\xa9\xaa\xab\xac\xad\xae\xaf\xb0\xb1\xb2\xb3\xb4\xb5\xb6\xb7\xb8\xb9\xba\xbb\xbc\xbd\xbe\xbf\xc0\xc1\xc2\xc3\xc4\xc5\xc6\xc7\xc8\xc9\xca\xcb\xcc\xcd\xce\xcf\xd0\xd1\xd2\xd3\xd4\xd5\xd6\xd7\xd8\xd9\xda\xdb\xdc\xdd\xde\xdf\xe0\xe1\xe2\xe3\xe4\xe5\xe6\xe7\xe8\xe9\xea\xeb\xec\xed\xee\xef\xf0\xf1\xf2\xf3\xf4\xf5\xf6\xf7\xf8\xf9\xfa\xfb\xfc\xfd\xfe\xff'
        var = 'xyzabcdef'
        assert var.translate(table, 'def') == 'xyzxyz'

        #table = string.maketrans('a', 'A')
        table = '\x00\x01\x02\x03\x04\x05\x06\x07\x08\t\n\x0b\x0c\r\x0e\x0f\x10\x11\x12\x13\x14\x15\x16\x17\x18\x19\x1a\x1b\x1c\x1d\x1e\x1f !"#$%&\'()*+,-./0123456789:;<=>?@ABCDEFGHIJKLMNOPQRSTUVWXYZ[\\]^_`Abcdefghijklmnopqrstuvwxyz{|}~\x7f\x80\x81\x82\x83\x84\x85\x86\x87\x88\x89\x8a\x8b\x8c\x8d\x8e\x8f\x90\x91\x92\x93\x94\x95\x96\x97\x98\x99\x9a\x9b\x9c\x9d\x9e\x9f\xa0\xa1\xa2\xa3\xa4\xa5\xa6\xa7\xa8\xa9\xaa\xab\xac\xad\xae\xaf\xb0\xb1\xb2\xb3\xb4\xb5\xb6\xb7\xb8\xb9\xba\xbb\xbc\xbd\xbe\xbf\xc0\xc1\xc2\xc3\xc4\xc5\xc6\xc7\xc8\xc9\xca\xcb\xcc\xcd\xce\xcf\xd0\xd1\xd2\xd3\xd4\xd5\xd6\xd7\xd8\xd9\xda\xdb\xdc\xdd\xde\xdf\xe0\xe1\xe2\xe3\xe4\xe5\xe6\xe7\xe8\xe9\xea\xeb\xec\xed\xee\xef\xf0\xf1\xf2\xf3\xf4\xf5\xf6\xf7\xf8\xf9\xfa\xfb\xfc\xfd\xfe\xff'
        var = 'abc'
        assert var.translate(table) == 'Abc'
        var = 'xyz'
        assert var.translate(table) == 'xyz'
        var = 'xyz'
        assert var.translate(table, 'x') == 'yz'
        # TODO: doesnt work for Pyston for some reason...
        #var = 'zyzzx'
        #assert var.translate(None, 'z') == 'yx'
        var = 'zyzzx'
        assert var.translate(None, '') == 'zyzzx'
        var = 'zyzzx'
        assert var.translate(None) == 'zyzzx'

        #self.checkraises(ValueError, 'xyz', 'translate', 'too short', 'strip')
        #self.checkraises(ValueError, 'xyz', 'translate', 'too short')


class MixinStrUserStringTest(object):
    ## Additional tests that only work with
    ## 8bit compatible object, i.e. str and UserString

    #if test_support.have_unicode:
        def test_encoding_decoding(self):
            #codecs = [('rot13', 'uryyb jbeyq'),
                      #('base64', 'aGVsbG8gd29ybGQ=\n'),
                      #('hex', '68656c6c6f20776f726c64'),
                      #('uu', 'begin 666 <data>\n+:&5L;&\\@=V]R;&0 \n \nend\n')]
            #for encoding, data in codecs:
                #self.checkequal(data, 'hello world', 'encode', encoding)
                var = data
                assert var.decode(encoding) == 'hello world' #self.checkequal('hello world', data, 'decode', encoding)
            ## zlib is optional, so we make the test optional too...
            #try:
                #import zlib
            #except ImportError:
                #pass
            #else:
                #data = 'x\x9c\xcbH\xcd\xc9\xc9W(\xcf/\xcaI\x01\x00\x1a\x0b\x04]'
                #self.checkequal(data, 'hello world', 'encode', 'zlib')
                var = data
                assert var.decode('zlib') == 'hello world' #self.checkequal('hello world', data, 'decode', 'zlib')

            #self.checkraises(TypeError, 'xyz', 'decode', 42)
            #self.checkraises(TypeError, 'xyz', 'encode', 42)

ct = CommonTest()
ct.test_capitalize()
ct.test_count()
ct.test_find()
ct.test_rfind()
ct.test_index()
ct.test_rindex()
ct.test_lower()
ct.test_upper()
ct.test_expandtabs()
ct.test_split()
ct.test_rsplit()
ct.test_strip()
ct.test_ljust()
ct.test_rjust()
ct.test_center()
ct.test_swapcase()
ct.test_replace()
#ct.test_replace_overflow()
ct.test_zfill()

msuust = MixinStrUnicodeUserStringTest()
# TODO: needs to be fixed...
#msuust.test_islower()
msuust.test_isupper()
msuust.test_istitle()
msuust.test_isspace()
msuust.test_isalpha()
msuust.test_isalnum()
msuust.test_isdigit()
msuust.test_title()
msuust.test_splitlines()
msuust.test_startswith()
msuust.test_endswith()
msuust.test___contains__()
msuust.test_subscript()
msuust.test_slice()
#msuust.test_extended_getslice()
msuust.test_mul()
msuust.test_join()
#msuust.test_formatting()
#msuust.test_floatformatting()
#msuust.test_inplace_rewrites()
msuust.test_partition()
msuust.test_rpartition()
msuust.test_none_arguments()
#msuust.test_find_etc_raise_correct_error_messages()

mssust = MixinStrStringUserStringTest()
mssust.test_translate()

