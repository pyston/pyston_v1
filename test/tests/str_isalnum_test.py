var = ''
assert var.isalnum() == False

var = 'a'
assert var.isalnum() == True

var = 'A'
assert var.isalnum() == True

var = '\n'
assert var.isalnum() == False

var = '123abc456'
assert var.isalnum() == True

var = 'a1b3c'
assert var.isalnum() == True

var = 'aBc000 '
assert var.isalnum() == False

var = 'abc\n'
assert var.isalnum() == False

try:
    var = 'abc'
    var.isalnum(42)
    assert False
except TypeError:
    assert True

