
# Testing capitalize/title methods.

def test(s):
    print 'string:', repr(s), 'capitalize:', s.capitalize(), 'title:', s.title()

test(' hello ')
test('Hello ')
test('hello ')
test('aaaa')
test('AaAa')
test('fOrMaT thIs aS title String')
test('fOrMaT,thIs-aS*title;String')
test('getInt')

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
