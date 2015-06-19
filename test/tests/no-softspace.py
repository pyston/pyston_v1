
import sys

import hashlib
class HashOutput:
    def __init__(self):
        self.m = hashlib.md5()
    def write(self, string):
        self.m.update(string)
    def md5hash(self):
        return self.m.hexdigest()

hash_output = HashOutput()
old_stdout = sys.stdout
sys.stdout = hash_output

print "Hello World!"

sys.stdout = old_stdout
print hash_output.md5hash()
