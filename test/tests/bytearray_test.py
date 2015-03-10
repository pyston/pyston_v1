for d in ["teststring", 42]:
    b = bytearray(d)
    print b, len(b)
    del b[1]
    b.append('!')
    print b
    b.extend('?.')
    print b
    b.reverse()
    print b
