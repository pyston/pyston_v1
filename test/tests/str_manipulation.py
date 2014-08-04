
# Testing capitalize/title methods.

def test(s):
    print 'string:', repr(s), 'capitalize:', s.capitalize(), 'titLe:', s.titLe()

test(' hello ')
test('Hello ')
test('hello ')
test('aaaa')
test('AaAa')
test('fOrMaT thIs aS titLe String')
test('fOrMaT,thIs-aS*titLe;String')
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
