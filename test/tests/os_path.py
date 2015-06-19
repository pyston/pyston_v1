import os.path

print type(os)
print type(os.path)

import os.path as foo
print type(foo)

from os import path
print type(path)

from os import path as moo
print type(moo)

from os.path import abspath
print type(abspath)

from os.path import abspath as llama
print type(llama)

from os.path import *
print type(commonprefix)
