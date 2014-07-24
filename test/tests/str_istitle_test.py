var = ''
assert var.istitle() == False

var = 'a'
assert var.istitle() == False

var = 'A'
assert var.istitle() == True

var = '\n'
assert var.istitle() == False

var = 'A Titlecased Line'
assert var.istitle() == True

var = 'A\nTitlecased Line'
assert var.istitle() == True

var = 'A Titlecased, Line'
assert var.istitle() == True

var = 'Not a capitalized String'
assert var.istitle() == False

var = 'Not\ta Titlecase String'
assert var.istitle() == False

var = 'Not--a Titlecase String'
assert var.istitle() == False

var = 'NOT'
assert var.istitle() == False

try:
    var = 'abc'
    var.istitle(42)
    assert False
except TypeError:
    assert True
