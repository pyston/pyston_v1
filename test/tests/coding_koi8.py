# -*- coding: koi8-r -*-
def test(s):
    print s, "len:", len(s)
    for c in s:
        print hex(ord(c)),
    print ""
test(u"Питон".encode("utf8"))
