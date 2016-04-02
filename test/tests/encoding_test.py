import codecs

### Codec APIs

class MyUnicode(unicode):
    def __new__(*args):
        print "MyUnicode.__new__", map(type, args)
        return unicode.__new__(*args)

    def __init__(*args):
        print "MyUnicode.__init__", map(type, args)

def encode(input, errors='strict'):
    raise Exception()

def decode(input, errors='strict'):
    return (MyUnicode(u"."), 1)

class IncrementalEncoder(codecs.IncrementalEncoder):
    def encode(self, input, final=False):
        return codecs.utf_8_encode(input, self.errors)[0]

class IncrementalDecoder(codecs.BufferedIncrementalDecoder):
    _buffer_decode = codecs.utf_8_decode

class StreamWriter(codecs.StreamWriter):
    encode = codecs.utf_8_encode

class StreamReader(codecs.StreamReader):
    decode = codecs.utf_8_decode

codec = codecs.CodecInfo(
    name='myunicode',
    encode=encode,
    decode=decode,
    incrementalencoder=IncrementalEncoder,
    incrementaldecoder=IncrementalDecoder,
    streamreader=StreamReader,
    streamwriter=StreamWriter,
)

def search(name):
    if name == "myunicode":
        return codec
codecs.register(search)


u = unicode("hello world", "myunicode", "strict")
print type(u)
