def f():
    x = 10.0 ** 2
    print locals()
    # Uncommenting this avoids the crash since we will keep x alive:
    # print x
f()
