class C(object):
    def __init__(self, a):
        print a

C(1)
C(a=2)
C(*(3,))
C(**{'a':4})
