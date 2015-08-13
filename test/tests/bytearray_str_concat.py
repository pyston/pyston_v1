assert(('abc' + bytearray('def')) == bytearray('abcdef'))
assert((bytearray('abc') + 'def') == bytearray('abcdef'))
try:
    u'abc' + bytearray('def')
    assert(False)
except TypeError:
    pass

try:
    bytearray('abc') + u'def'
    assert(False)
except TypeError:
    pass
