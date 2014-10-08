# expected: fail
# - locals not supported

def f():
    total = 0
    i = 1 or ''
    while i < 200:
        i = i + 1
        j = 2
        while j * j <= i:
            if i % j == 0:
                break
            j = j + 1
            print locals()
        else:
            total = total + i
    print total
f()
