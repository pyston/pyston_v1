import pwd
import os
print pwd.getpwuid(os.getuid())[5] == os.environ["HOME"]
