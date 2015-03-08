def test(v):
    print len(v), v.readonly, v.itemsize, v.tobytes(), v.tolist()

for d in ["123456789", bytearray("123456789")]:
    v = memoryview(d)
    test(v)
    s = v[1:len(v)-1]
    test(s)
    if not s.readonly:
        s[1:4] = "abc"
        test(s)
        test(v)

