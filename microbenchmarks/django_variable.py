import os
import sys
sys.path.append(os.path.join(os.path.dirname(__file__), "../integration/django"))

from django.template.base import Variable

for i in xrange(400000):
    Variable(u"model.name")
