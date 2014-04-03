# run_args: -n
def foo():
    print "foo"

def bar():
    print "bar"

l = [foo, foo, bar, bar]
for i in l:
    i()
