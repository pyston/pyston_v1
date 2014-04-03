# icsetattr-newattr test

n = 10
while n:
    def f():
        pass

    n = n - 1
    f.x = n
    print f.x
