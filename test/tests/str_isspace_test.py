var = ''
assert var.isspace() == False

var = 'a'
assert var.isspace() == False

var = ' '
assert var.isspace() == True

var = '\t'
assert var.isspace() == True

var = '\r'
assert var.isspace() == True

var = '\n'
assert var.isspace() == True

var = ' \t\r\n'
assert var.isspace() == True

var = ' \t\r\na'
assert var.isspace() == False

try:
    var = 'abc'
    var.isspace(42)
    assert False
except TypeError:
    assert True
