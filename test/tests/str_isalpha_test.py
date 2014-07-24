var = ''
assert var.isalpha() == False

var = 'a'
assert var.isalpha() == True

var = 'A'
assert var.isalpha() == True

var = '\n'
assert var.isalpha() == False

var = 'abc'
assert var.isalpha() == True

var = 'aBc123'
assert var.isalpha() == False

var = 'abc\n'
assert var.isalpha() == False

try:
    var = 'abc'
    var.isalpha(42)
    assert False
except TypeError:
    assert True
