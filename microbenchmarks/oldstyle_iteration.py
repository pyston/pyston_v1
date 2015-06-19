class C:
    def __init__(self):
        self.l = range(10)

    def __getitem__(self, idx):
        return self.l[idx]

def f():
    c = C()
    total = 0
    for _ in xrange(100000):
        for i in c:
            total += i
    print total

f()

