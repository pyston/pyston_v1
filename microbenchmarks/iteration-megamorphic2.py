class Iterable(object):
    def __iter__(self):
        return self

    def __hasnext__(self):
        False

    def next(self):
        raise StopIteration()


def do_iter(r):
    for i in r:
        pass


i1 = Iterable()

f = 0
while f < 100000:
    class IterableSub(Iterable):
        pass
    i2 = IterableSub()
    f += 1
    do_iter(i1)
    do_iter(i2)
