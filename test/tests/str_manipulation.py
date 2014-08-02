
# Tests for lower, upper and swapcase

def test_lower_upper_swapcase(s):
    print repr(s), repr(s.lower()), repr(s.upper()), repr(s.swapcase())

test_lower_upper_swapcase('HeLLo')
test_lower_upper_swapcase('hello')
test_lower_upper_swapcase('PoZdRaWiAm AgATE')
test_lower_upper_swapcase('KrZySIU jem zUPe')
test_lower_upper_swapcase('PchnAc W te LOdZ JeZa lUb OsM SkRzyN Fig\n')
test_lower_upper_swapcase('HeLLo cOmpUteRs')
test_lower_upper_swapcase('hEllO CoMPuTErS')

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
