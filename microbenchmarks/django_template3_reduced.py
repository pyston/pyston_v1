import os
import sys

sys.path.append(os.path.join(os.path.dirname(__file__), "../test/lib/django"))
BENCHMARK_SUITE_DIR = os.path.join(os.path.dirname(__file__), "../../pyston-perf/benchmarking/benchmark_suite")
sys.path.extend([os.path.join(BENCHMARK_SUITE_DIR, "django_template2_site")])

from django.template.base import Origin, Template, Context, TemplateDoesNotExist
from django.conf import settings
from django.apps import apps
import time
import shutil

# Copy the "base" db so we always start with a knownn state:
os.environ.setdefault("DJANGO_SETTINGS_MODULE", "testsite.settings")

try:
    import __pyston__
    pyston_loaded = True
except:
    pyston_loaded = False

template_source = """
{% load admin_static %}{% load firstof from future %}<!DOCTYPE html>
<html lang="{{ LANGUAGE_CODE|default:"en-us" }}" {% if LANGUAGE_BIDI %}dir="rtl"{% endif %}>
<head>
<title>{% block title %}{% endblock %}</title>
<link rel="stylesheet" type="text/css" href="{% block stylesheet %}{% static "admin/css/base.css" %}{% endblock %}" />
{% block extrastyle %}{% endblock %}
<!--[if lte IE 7]><link rel="stylesheet" type="text/css" href="{% block stylesheet_ie %}{% static "admin/css/ie.css" %}{% endblock %}" /><![endif]-->
{% if LANGUAGE_BIDI %}<link rel="stylesheet" type="text/css" href="{% block stylesheet_rtl %}{% static "admin/css/rtl.css" %}{% endblock %}" />{% endif %}
<script type="text/javascript">window.__admin_media_prefix__ = "{% filter escapejs %}{% static "admin/" %}{% endfilter %}";</script>
<script type="text/javascript">window.__admin_utc_offset__ = "{% filter escapejs %}{% now "Z" %}{% endfilter %}";</script>
{% block extrahead %}{% endblock %}
{% block blockbots %}<meta name="robots" content="NONE,NOARCHIVE" />{% endblock %}
</head>
{% load i18n %}

<body class="{% if is_popup %}popup {% endif %}{% block bodyclass %}{% endblock %}">

<!-- Container -->
<div id="container">

    {% if not is_popup %}
    <!-- Header -->
    <div id="header">
        <div id="branding">
        {% block branding %}{% endblock %}
        </div>
        {% if user.is_active and user.is_staff %}
        <div id="user-tools">
            {% block welcome-msg %}
                {% trans 'Welcome,' %}
                <strong>{% firstof user.get_short_name user.get_username %}</strong>.
            {% endblock %}
            {% block userlinks %}
                {% url 'django-admindocs-docroot' as docsroot %}
                {% if docsroot %}
                    <a href="{{ docsroot }}">{% trans 'Documentation' %}</a> /
                {% endif %}
                {% if user.has_usable_password %}
                <a href="{% url 'admin:password_change' %}">{% trans 'Change password' %}</a> /
                {% endif %}
                <a href="{% url 'admin:logout' %}">{% trans 'Log out' %}</a>
            {% endblock %}
        </div>
        {% endif %}
        {% block nav-global %}{% endblock %}
    </div>
    <!-- END Header -->
    {% block breadcrumbs %}
    <div class="breadcrumbs">
    <a href="{% url 'admin:index' %}">{% trans 'Home' %}</a>
    {% if title %} &rsaquo; {{ title }}{% endif %}
    </div>
    {% endblock %}
    {% endif %}

    {% block messages %}
        {% if messages %}
        <ul class="messagelist">{% for message in messages %}
          <li{% if message.tags %} class="{{ message.tags }}"{% endif %}>{{ message|capfirst }}</li>
        {% endfor %}</ul>
        {% endif %}
    {% endblock messages %}

    <!-- Content -->
    <div id="content" class="{% block coltype %}colM{% endblock %}">
        {% block pretitle %}{% endblock %}
        {% block content_title %}{% if title %}<h1>{{ title }}</h1>{% endif %}{% endblock %}
        {% block content %}
        {% block object-tools %}{% endblock %}
        {{ content }}
        {% endblock %}
        {% block sidebar %}{% endblock %}
        <br class="clear" />
    </div>
    <!-- END Content -->

    {% block footer %}<div id="footer"></div>{% endblock %}
</div>
<!-- END Container -->

</body>
</html>
"""

apps.populate((
    'django.contrib.admin',
    'django.contrib.auth',
    'django.contrib.contenttypes',
    'django.contrib.sessions',
    'django.contrib.messages',
    'django.contrib.staticfiles',
))

settings.TEMPLATE_LOADERS = (
    ('django.template.loaders.cached.Loader', (
        'django.template.loaders.filesystem.Loader',
        'django.template.loaders.app_directories.Loader',
    )),
)

elapsed = 0
template = Template(template_source, None, "admin/index.html")

d = {}
from django.contrib.auth.models import User
d['user'] = User(2)
# This list was created by running an empty django instance and seeing what it passed for app_list:
d['app_list'] = [{'app_url': '/admin/auth/', 'models': [{'perms': {'add': True, 'change': True, 'delete': True}, 'admin_url': '/admin/auth/group/', 'object_name': 'Group', 'name': "<name>", 'add_url': '/admin/auth/group/add/'}, {'perms': {'add': True, 'change': True, 'delete': True}, 'admin_url': '/admin/auth/user/', 'object_name': 'User', 'name': "<name>", 'add_url': '/admin/auth/user/add/'}], 'has_module_perms': True, 'name': "<name>", 'app_label': 'auth'}]
context = Context(d)

def measure_iters():
    for i in xrange(6000):
        start = time.time()
        template.render(context)
        elapsed = time.time() - start
        print elapsed
    print "took %4.1fms for last iteration" % (elapsed * 1000.0,)

def measure_by_nodetype():
    times = {}
    for i in xrange(6000):
        for n in template.nodelist:
            start = time.time()
            n.render(context)
            elapsed = time.time() - start
            times[type(n)] = times.get(type(n), 0) + elapsed
    for k, v in sorted(times.items(), key=lambda (k,v):k.__name__):
        print k.__name__, v
measure_by_nodetype()
