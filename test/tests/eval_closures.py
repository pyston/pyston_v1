# expected: fail
# - eval not implemented
# - closures not implemented
x = 2
def wrap():
    x = 1
    y = 3

    # The eval('x') in this function will resolve to the global scope:
    def inner1():
        y
        print locals()
        print eval('x')
    inner1()

    # The eval('x') in this function will resolve to the closure, since
    # there is a textual reference to x which causes it to get captured:
    def inner2():
        x
        print locals()
        print eval('x')
    inner2()

wrap()
