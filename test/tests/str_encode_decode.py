def test(string, encoding):
 s = string.encode(encoding)
 print encoding, s
 assert string == s.decode(encoding)

test("hello world", "hex")
test("hello world", "base64")
test("\r\n\\", "string-escape")

"".encode()
"".decode()
u"".encode()
u"".decode()
