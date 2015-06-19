
# This throws an exception in the import machinery when we try to access __path__,
# but that should get caught.
# Also, email.MIMEText isn't even a Module or a subclass of Module...
from email.MIMEText import MIMEText

from email.MIMEMultipart import MIMEMultipart

# TODO check similar cases with descriptors
