try:
    import _sha as sha
except ImportError:
    import sha

s = sha.new()
print s.hexdigest()
s.update("aoeu")
print s.hexdigest()
