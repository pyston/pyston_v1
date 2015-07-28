import multiprocessing

# from https://docs.python.org/2/library/multiprocessing.html

def f(x):
    return x*x

if __name__ == '__main__':
    p = multiprocessing.Pool(5)
    print(p.map(f, [1, 2, 3]))


def f(name):
    print 'hello', name

if __name__ == '__main__':
    p = multiprocessing.Process(target=f, args=('bob',))
    p.start()
    p.join()
    print p.exitcode


def f(q):
    q.put([42, None, 'hello'])

if __name__ == '__main__':
    q = multiprocessing.Queue()
    p = multiprocessing.Process(target=f, args=(q,))
    p.start()
    print q.get()    # prints "[42, None, 'hello']"
    p.join()
    print p.exitcode
print "done"
