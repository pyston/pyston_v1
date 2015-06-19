# expected: fail
# - we don't support the cmp= keyword yet
# (apparently it's removed in Python 3)

printed = False
def mycmp(x, y):
    global printed
    if not printed:
        print type(x), type(y)
        printed = True

    if x < y:
        return -1
    if x > y:
        return 1
    return 0

l = range(9)
print l.sort(cmp=mycmp, key=lambda x:x%2, reverse=True)
print l
