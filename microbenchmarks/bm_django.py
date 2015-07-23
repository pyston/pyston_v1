"""
This benchmark was adapted from
https://bitbucket.org/pypy/benchmarks/src/34f06618ef7f29d72d9c19f1e3894607587edc76/unladen_swallow/performance/bm_django.py?at=default
"""

__author__ = "collinwinter@google.com (Collin Winter)"

import os
import sys
sys.path.append(os.path.join(os.path.dirname(__file__), "../test/integration/django"))

from django.conf import settings
settings.configure()
from django.template import Context, Template

import time

DJANGO_TMPL = Template("""<table>
{% for row in table %}
<tr>{% for col in row %}<td>{{ col|escape }}</td>{% endfor %}</tr>
{% endfor %}
</table>
""")

def test_django(count):
    table = [xrange(50) for _ in xrange(50)]
    context = Context({"table": table})

    times = []
    for _ in xrange(count):
        t0 = time.time()
        data = DJANGO_TMPL.render(context)
        t1 = time.time()
        times.append(t1 - t0)
    return times

test_django(15)
