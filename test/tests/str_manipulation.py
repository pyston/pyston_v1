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

for i in xrange(256):
    c = chr(i)
    s = "a%sb" % c
    if s.title()[2] == 'b':
        print repr(c)

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
