# Testing capitalize/title methods.

def test(s):
    print 'string:', repr(s), repr(s.lower()), repr(s.upper()), repr(s.swapcase()), repr(s.capitalize()), repr(s.title())

test(' hello ')
test('Hello ')
test('hello ')
test('aaaa')
test('AaAa')
test('fOrMaT thIs aS title String')
test('fOrMaT,thIs-aS*title;String')
test('getInt')
test('PoZdRaWiAm AgATE')
test('KrZySIU jem zUPe')
test('PchnAc W te LOdZ JeZa lUb OsM SkRzyN Fig\n')
test('HeLLo cOmpUteRs')
test('hEllO CoMPuTErS')


var = 'hello'
try:
   var.capitalize(42)
   print 'TypeError not raised'
except TypeError:
   print 'TypeError raised'

var = 'hello'
try:
   var.title(42)
   print 'TypeError not raised'
except TypeError:
   print 'TypeError raised'

try:
    var = 'hello'
    var.lower(42)
    print 'TypeError not raised'
except TypeError:
    print 'TypeError raised'

try:
    var = 'hello'
    var.upper(42)
    print 'TypeError not raised'
except TypeError:
    print 'TypeError raised'

try:
    var = 'hello'
    var.swapcase(42)
    print 'TypeError not raised'
except TypeError:
    print 'TypeError raised'

# Testing isalnum, isalpha, isdigit, islower, isspace, istitle and isupper methods

def test_is(s):
    print 'string:', repr(s), 'isalnum:', s.isalnum(), 'isalpha:', s.isalpha(), 'isdigit:', s.isdigit(), 'islower:', s.islower(), 'isspace:', s.isspace(), 'istitle:', s.istitle(), 'isupper:', s.isupper()

test_is('')
test_is('a')
test_is('A')
test_is('123abc456')
test_is('a1b3c')
test_is('aBc000 ')
test_is('abc\n')
test_is('aBc123')
test_is('abc')
test_is('0')
test_is('0123456789')
test_is('0123456789a')
test_is(' ')
test_is('\t')
test_is('\r')
test_is('\n')
test_is(' \t\r\n')
test_is(' \t\r\na')
test_is('A Titlecased Line')
test_is('A\nTitlecased Line')
test_is('A Titlecased, Line')
test_is('Not a capitalized String')
test_is('Not\ta Titlecase String')
test_is('Not--a Titlecase String')
test_is('NOT')

for i in xrange(256):
    c = chr(i)
    s = "a%sb" % c
    if s.title()[2] == 'b':
        print repr(c)

    test(c)
    test_is(c)

    for j in xrange(i, 64):
        test_is(c + chr(j))

try:
    var = 'abc'
    var.isalnum(42)
    print 'TypeError not raised'
except TypeError:
    print 'TypeError raised'


try:
    var = 'abc'
    var.isalpha(42)
    print 'TypeError not raised'
except TypeError:
    print 'TypeError raised'

try:
    var = 'abc'
    var.isdigit(42)
    print 'TypeError not raised'
except TypeError:
    print 'TypeError raised'

try:
    var = 'abc'
    var.islower(42)
    print 'TypeError not raised'
except TypeError:
    print 'TypeError raised'

try:
    var = 'abc'
    var.isspace(42)
    print 'TypeError not raised'
except TypeError:
    print 'TypeError raised'

try:
    var = 'abc'
    var.istitle(42)
    print 'TypeError not raised'
except TypeError:
    print 'TypeError raised'


try:
    var = 'abc'
    var.isupper(42)
    print 'TypeError not raised'
except TypeError:
    print 'TypeError raised'

def f(s):
    if s.isalnum():
        pass
    if s.isalpha():
        pass
    if s.isdigit():
        pass
    if s.islower():
        pass
    if s.isspace():
        pass
    if s.istitle():
        pass
    if s.isupper():
        pass
f("")
f("1")
