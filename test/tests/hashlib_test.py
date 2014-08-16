import hashlib

#for m in [hashlib.md5(), hashlib.sha1(), hashlib.sha256(), hashlib.sha512()]:
for m in [hashlib.sha256(), hashlib.sha512()]:
 m.update("pyston")
 print m.digest_size, m.hexdigest()
