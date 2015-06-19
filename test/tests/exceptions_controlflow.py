# Regression test:
for i in xrange(5):
    try:
        for j in xrange(5):
            print i, j
    except:
        pass



# Another regression test:

# Function to hide types:
def ident(x):
    return x

def f2(x, y):
    x = ident(x)
    y = ident(y)

    try:
        for i in xrange(10):
            x += y
    except Exception, e:
        print e
    print x
f2(1, 2)
