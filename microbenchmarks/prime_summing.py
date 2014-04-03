def f():
    total = 0
    i = 1
    while i < 2000000:
        i = i + 1
        j = 2
        while j * j <= i:
            if i % j == 0:
                break
            j = j + 1
        else:
            total = total + i
    print total
f()
