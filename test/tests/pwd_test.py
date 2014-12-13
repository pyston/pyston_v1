# allow-warning: converting unicode literal to str

import pwd
import os
print pwd.getpwuid(os.getuid())[5] == os.environ["HOME"]
