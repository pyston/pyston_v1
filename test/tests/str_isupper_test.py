var = ''
assert var.isupper() == False

var = 'a'
assert var.isupper() == False

var = 'A'
assert var.isupper() == True

var = '\n'
assert var.isupper() == False

var = 'ABC'
assert var.isupper() == True

var = 'AbC'
assert var.isupper() == False

# TODO: not supported yet
#var = 'ABC\n'
#assert var.isupper() == True

try:
    var = 'abc'
    var.isupper(42)
    assert False
except TypeError:
    assert True
