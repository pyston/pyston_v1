

assert chr(0) == '\x00'
assert chr(1) == '\x01'
assert chr(128) == '\x80'
assert chr(255) == '\xff'

try:
    chr('a')
    assert False
except TypeError as e:
    assert e.message == 'an integer is required'

try:
    chr(256)
    assert 'chr(256) should throw chr() arg not in range(256)' == False
except ValueError as e:
    assert e.message == 'chr() arg not in range(256)'

try:
    chr(-1)
    assert 'chr(-1) should throw chr() arg not in range(256)' == False
except ValueError as e:
    assert e.message == 'chr() arg not in range(256)'
