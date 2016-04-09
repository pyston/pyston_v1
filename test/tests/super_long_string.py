import os
import sys

s = 'def f():\n    return """' + '.' * 100000 + '"""'

path = os.path.join(os.path.dirname(__file__), "super_long_string_target.py")

with open(path, 'w') as f:
    f.write(s)

import super_long_string_target
del sys.modules["super_long_string_target"]
import super_long_string_target

os.remove(path)
os.remove(path + 'c')
