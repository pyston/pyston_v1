# skip-if: True
# - this is blocking on some rewriter stuff

import urlparse
print urlparse.urlparse("http://www.dropbox.com")
