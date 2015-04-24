# skip-if: True

import sys
print "import_failure_target" in sys.modules
raise Exception("pretending submodule didn't import")
