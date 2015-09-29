# skip-if: True
import multiprocessing

# from https://docs.python.org/2/library/multiprocessing.html

def f(n, a):
    n.value = 3.1415927
    for i in range(len(a)):
        a[i] = -a[i]

if __name__ == '__main__':
    num = multiprocessing.Value('d', 0.0)
    arr = multiprocessing.Array('i', range(10))

    p = multiprocessing.Process(target=f, args=(num, arr))
    p.start()
    p.join()

    print num.value
    print arr[:]
