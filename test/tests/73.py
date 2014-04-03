# slice slicing

l = [2, 3, 5, 7]

# exhaustively try all index pairs:
i = -5
while i < 6:
    print "a", i, l[i:]
    print range(i)
    print "b", i, l[i::]
    print "c", i, l[:i]
    print "d", i, l[:i:]
    if i != 0:
        print "e", i, l[::i]

    j = -5
    while j < 6:
        print "f", i, j, l[i:j]
        print range(i, j)
        print "g", i, j, l[i:j:]
        if j != 0:
            print "h", i, j, l[i::j]
            print "i", i, j, l[:i:j]

        k = -5
        while k < 6:
            if k != 0:
                print "l", i, j, k, l[i:j:k]
                print range(i, j, k)
            k = k + 1
        j = j + 1
    i = i + 1


print "hello world"[:5]
