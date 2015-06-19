def f():
    print (1,2,3)[-1]
f()

def f2():
    try:
        print (1,2,3)[4]
    except IndexError as e:
        print e

    try:
        print (1,2,3)[-4]
    except IndexError as e:
        print e
f2()

def f3():
    try:
        print (1,2,3)['hello']
    except TypeError as e:
        print e
f3()
