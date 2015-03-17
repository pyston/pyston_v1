from StringIO import StringIO
import json

# encoding basic python object hierarchies
print 1, json.dumps("")
print 2, json.dumps(['foo', {'bar': ('baz', None, 1.0, 2)}])
print 3, json.dumps("\"foo\bar")
print 4, json.dumps(u'\u1234')
print 5, json.dumps('\\')
print 6, json.dumps({"c": 0, "b": 0, "a": 0}, sort_keys=True)
io = StringIO()
json.dump(['streaming API'], io)
print 7, io.getvalue()

# compact encoding
print 8, json.dumps([1,2,3,{'4': 5, '6': 7}], sort_keys=True, separators=(',',':'))

# pretty printing
print 9, json.dumps({'4': 5, '6': 7}, sort_keys=True, indent=4, separators=(',', ': '))

# decoding json

print 10, json.loads('["foo", {"bar":["baz", null, 1.0, 2]}]')
print 11, json.loads('"\\"foo\\bar"')
io = StringIO('["streaming API"]')
print 12, json.load(io)

# specializing json object decoding
def as_complex(dct):
    if '__complex__' in dct:
        return complex(dct['real'], dct['imag'])
    return dct

# disable this part for now.  pyston formats the real/imaginary parts as floats always, but cpython
# formats them as integers if they are.  i.e. (1+2j) in cpython vs (1.0+2.0j) in pyston.
#print 13, json.loads('{"__complex__": true, "real": 1, "imag": 2}', object_hook=as_complex)

# Pyston doesn't support the decimal module yet
#import decimal
#print type(json.loads('1.1', parse_float=decimal.Decimal))

# extending JSONEncoder

class ComplexEncoder(json.JSONEncoder):
    def default(self, obj):
        if isinstance(obj, complex):
            return [obj.real, obj.imag]
        # Let the base class default method raise the TypeError
        return json.JSONEncoder.default(self, obj)

print 14, json.dumps(complex(2, 1), cls=ComplexEncoder)
print 15, ComplexEncoder().encode(complex(2, 1))
print 16, list(ComplexEncoder().iterencode(complex(2, 1)))

