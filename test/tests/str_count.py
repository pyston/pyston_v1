import sys

# Testing count

def test_count():
    var = 'aaa'
    print repr(var.count('a'))
    print repr(var.count('b'))
    print repr(var.count('a'))
    print repr(var.count('b'))
    print repr(var.count('a'))
    print repr(var.count('b'))
    print repr(var.count('a', 1))
    print repr(var.count('a', 10))
    print repr(var.count('a', -1))
    print repr(var.count('a', -10))
    print repr(var.count('a', 0, 1))
    print repr(var.count('a', 0, 10))
    print repr(var.count('a', 0, -1))
    print repr(var.count('a', 0, -10))
    print repr(var.count('', 1))
    print repr(var.count('', 3))
    print repr(var.count('', 10))
    print repr(var.count('', -1))
    print repr(var.count('', -10))

    var = ''
    print repr(var.count(''))
    print repr(var.count('', 1, 1))
    # TODO: doesnt work yet...
    #print repr(var.count('', sys.maxint, 0))
    print repr(var.count('xx'))
    print repr(var.count('xx', 1, 1))
    print repr(var.count('xx', sys.maxint, 0))

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

    # For a variety of combinations,
    #    verify that str.count() matches an equivalent function
    #    replacing all occurrences and then differencing the string lengths
    charset = ['', 'a', 'b']
    digits = 7
    base = len(charset)
    teststrings = set()
    for i in xrange(base ** digits):
        entry = []
        for j in xrange(digits):
            i, m = divmod(i, base)
            entry.append(charset[m])
        teststrings.add(''.join(entry))
    teststrings = list(teststrings)
    for i in teststrings:
        n = len(i)
        for j in teststrings:
            r1 = i.count(j)
            if j:
                r2, rem = divmod(n - len(i.replace(j, '')), len(j))
            else:
                r2, rem = len(i)+1, 0
            if rem or r1 != r2:
                print '%s != 0 for %s' % (rem, i)
                print rem == 0
                print '%s != %s for %s' % (r1, r2, i)
                print r1 == r2

test_count()
 
