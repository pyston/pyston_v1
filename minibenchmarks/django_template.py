import os
import sys
sys.path.append(os.path.join(os.path.dirname(__file__), "../test/integration/django"))

from django.template.base import Origin, Template, Context, TemplateDoesNotExist
from django.conf import settings
from django.apps import apps
import time

try:
    import __pyston__
    pyston_loaded = True
except:
    pyston_loaded = False

template_source = """
{% extends "admin/base_site.html" %}
{% load i18n admin_static %}

{% block extrastyle %}{{ block.super }}<link rel="stylesheet" type="text/css" href="{% static "admin/css/dashboard.css" %}" />{% endblock %}

{% block coltype %}colMS{% endblock %}

{% block bodyclass %}{{ block.super }} dashboard{% endblock %}

{% block breadcrumbs %}{% endblock %}

{% block content %}
<div id="content-main">

{% if app_list %}
    {% for app in app_list %}
        <div class="app-{{ app.app_label }} module">
        <table>
        <caption>
            <a href="{{ app.app_url }}" class="section" title="{% blocktrans with name=app.name %}Models in the {{ name }} application{% endblocktrans %}">{{ app.name }}</a>
        </caption>
        {% for model in app.models %}
            <tr class="model-{{ model.object_name|lower }}">
            {% if model.admin_url %}
                <th scope="row"><a href="{{ model.admin_url }}">{{ model.name }}</a></th>
            {% else %}
                <th scope="row">{{ model.name }}</th>
            {% endif %}

            {% if model.add_url %}
                <td><a href="{{ model.add_url }}" class="addlink">{% trans 'Add' %}</a></td>
            {% else %}
                <td>&nbsp;</td>
            {% endif %}

            {% if model.admin_url %}
                <td><a href="{{ model.admin_url }}" class="changelink">{% trans 'Change' %}</a></td>
            {% else %}
                <td>&nbsp;</td>
            {% endif %}
            </tr>
        {% endfor %}
        </table>
        </div>
    {% endfor %}
{% else %}
    <p>{% trans "You don't have permission to edit anything." %}</p>
{% endif %}
</div>
{% endblock %}

{% block sidebar %}
<div id="content-related">
    <div class="module" id="recent-actions-module">
        <h2>{% trans 'Recent Actions' %}</h2>
        <h3>{% trans 'My Actions' %}</h3>
            {% load log %}
            {% get_admin_log 10 as admin_log for_user user %}
            {% if not admin_log %}
            <p>{% trans 'None available' %}</p>
            {% else %}
            <ul class="actionlist">
            {% for entry in admin_log %}
            <li class="{% if entry.is_addition %}addlink{% endif %}{% if entry.is_change %}changelink{% endif %}{% if entry.is_deletion %}deletelink{% endif %}">
                {% if entry.is_deletion or not entry.get_admin_url %}
                    {{ entry.object_repr }}
                {% else %}
                    <a href="{{ entry.get_admin_url }}">{{ entry.object_repr }}</a>
                {% endif %}
                <br/>
                {% if entry.content_type %}
                    <span class="mini quiet">{% filter capfirst %}{% trans entry.content_type.name %}{% endfilter %}</span>
                {% else %}
                    <span class="mini quiet">{% trans 'Unknown content' %}</span>
                {% endif %}
            </li>
            {% endfor %}
            </ul>
            {% endif %}
    </div>
</div>
{% endblock %}
"""

settings.configure()

apps.populate((
    'django.contrib.admin',
    'django.contrib.auth',
    'django.contrib.contenttypes',
    'django.contrib.sessions',
    'django.contrib.messages',
    'django.contrib.staticfiles',
))

elapsed = 0
for i in xrange(500):
    #if pyston_loaded:
    #    __pyston__.clearStats()
    start = time.time()
    template = Template(template_source, None, "admin/index.html")
    elapsed = time.time() - start
print "took %4.1fms for last iteration" % (elapsed * 1000.0,)
