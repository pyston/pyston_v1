# allow-warning: converting unicode literal to str
# allow-warning: import level 0 will be treated as -1!
def test(string, encoding):
 s = string.encode(encoding)
 print encoding, s
 assert string == s.decode(encoding)

test("hello world", "hex")
test("hello world", "base64")
test("\r\n\\", "string-escape")

