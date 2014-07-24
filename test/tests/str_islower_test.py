var = ''
assert var.islower() == False

var = 'a'
assert var.islower() == True

var = 'A'
assert var.islower() == False

var = '\n'
assert var.islower() == False

var = 'abc'
assert var.islower() == True

var = 'aBc'
assert var.islower() == False

# TODO: not supported yet
#var = 'abc\n'
#assert var.islower() == True

try:
    var = 'abc'
    var.islower(42)
    assert False
except TypeError:
    assert True
