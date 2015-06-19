try:
    import _md5 as md5
except ImportError:
    import md5

m = md5.new()
print m.hexdigest()
m.update("aoeu")
print m.hexdigest()
