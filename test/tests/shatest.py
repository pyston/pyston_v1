# expected: fail
# - warnings about PyString_AsString(), since that is allowed to be modified

try:
    import _sha as sha
except ImportError:
    import sha

s = sha.new()
print s.hexdigest()
s.update("aoeu")
print s.hexdigest()
