print repr(unicode())
print repr(unicode('hello world'))
print unicode('hello world')

# Some random unicode character:
u = u'\u0180'
print len(u)
print repr(u)
print repr(u.encode("utf8"))
print u

d = {}
d["hello world"] = "hi"
print d[u"hello world"]

class C(object):
    pass
c = C()
c.a = 1
print hasattr(c, 'a')
print hasattr(c, u'a')
print u'a' in c.__dict__
print u'' == ''
print '' == u''
print hash(u'') == hash('')
print hash(u'hello world') == hash('hello world')
print "Hello " + u" World"
print u"Hello " + " World"

def p(x):
    return [hex(ord(i)) for i in x]
s = u"\u20AC" # euro sign
print p(u"\N{EURO SIGN}")
print p(s) 
print p(s.encode("utf8"))
print p(s.encode("utf16"))
print p(s.encode("utf32"))
print p(s.encode("iso_8859_15"))
print p(s.encode(u"utf8"))
print s
print p("hello world".encode(u"utf8"))

print repr(u' '.join(["hello", "world"]))

# GC test: the unicode module interns certain unicode strings (the empty string among them).
# Make sure we don't end up GCing it.
# Call BaseException().__unicode__() since that happens to be one of the ways to access
# the interned empty string ("unicode_empty")
import gc
for i in xrange(100):
    print repr(BaseException().__unicode__())
    gc.collect()
    # do some allocations:
    for j in xrange(100):
        [None] * j

print u'' in ''
print '' in u''
print u'aoeu' in ''
print u'\u0180' in 'hello world'
print 'hello world' in u'\u0180'
print u''.__contains__('')
print ''.__contains__(u'')

class C(object):
    a = 1
    # We don't support this, with or without unicode:
    # locals()[u'b'] = 2
c = C()
print getattr(c, u'a')
# print c.b
c.__dict__[u'c'] = 3
print c.c
print getattr(c, u'c')
delattr(c, u'c')
print hasattr(c, u'c')

def f(a):
    print a
f(a=1)
f(**{'a':2})
f(**{u'a':3})

print repr('%s' % u'') # this gives a unicode object!

print repr('hello world'.replace(u'hello', u'hi'))

print "hello world".endswith(u'hello')
print "hello world".endswith(u'world')
print "hello world".startswith(u'hello')
print "hello world".startswith(u'world')

print float(u'1.0')

print unichr(97)
print unichr(23456)

print "hello world".split(u'l')
print "hello world".rsplit(u'l')

with open(u"/dev/null", u"r") as f:
    print f.read()

class CustomRepr(object):
    def __init__(self, x):
        self.x = x

    def __str__(self):
        return self.x

    def __repr__(self):
        return self.x

print repr(str(CustomRepr(u'')))
print repr(repr(CustomRepr(u'')))

try:
    str(CustomRepr(u'\u0180'))
except Exception as e:
    print type(e), e
try:
    repr(CustomRepr(u'\u0180'))
except Exception as e:
    print type(e), e
try:
    str(CustomRepr(1))
except Exception as e:
    print type(e), e
try:
    repr(CustomRepr(1))
except Exception as e:
    print type(e), e

class MyStr(str):
    pass

print type(str(CustomRepr(MyStr("hi"))))

print type(MyStr("hi").__str__())
print type(str(MyStr("hi")))

class C(object):
    def __repr__(self):
        return u"hello world"
print [C()], set([C()]), {1:C()}

print "hello" + u"world"

s = " \thello world\t "
for m in str.strip, str.lstrip, str.rstrip:
    for args in [], [" "], [u" "]:
        print repr(m(s, *args))

print "".join([u"\xB2", u"\xB3"])

import sys
print type(sys.maxunicode)

class MyUnicode(unicode):
    def __init__(*args):
        print "MyUnicode.__init__", map(type, args)

class C(object):
    def __unicode__(self):
        return MyUnicode("hello world")

print type(unicode(C()))
