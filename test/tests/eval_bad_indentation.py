try:
    eval("\n 2")
    print "bad, should have thrown an exception"
except SyntaxError:
    print "good, threw exception"
