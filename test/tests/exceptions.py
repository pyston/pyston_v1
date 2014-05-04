# expected: fail
# - exceptions

def throw(x):
    try:
        raise x
    except Exception, e:
        print type(e)

# Both print "Exception"
throw(Exception)
throw(Exception())
