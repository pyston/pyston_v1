# expected: fail
# - dict-backed objects don't keep track of their attrwrapper if they had one already.

class C(object):
    pass

c = C()
aw = c.__dict__

# Convert the object to be dict-backed:
aw[1] = 2

# __dict__ might return something else now:
print c.__dict__ is aw
