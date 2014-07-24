var = ''
assert var.isdigit() == False

var = 'a'
assert var.isdigit() == False

var = '0'
assert var.isdigit() == True

var = '0123456789'
assert var.isdigit() == True

var = '0123456789a'
assert var.isdigit() == False

try:
    var = 'abc'
    var.isdigit(42)
    assert False
except TypeError:
    assert True
