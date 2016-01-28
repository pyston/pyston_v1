#-*- coding: utf-8 -*-
#flask test
#todo: It should be tested as django_test.py.
import os, sys, subprocess, shutil
import multiprocessing, time, signal
import urllib2

sys.path.append(os.path.dirname(__file__) + "/../lib")
from test_helper import create_virtenv, run_test

ENV_NAME = "flask_test_env_" + os.path.basename(sys.executable)
create_virtenv(ENV_NAME, 
               ["flask == 0.10.1", 
                "Werkzeug == 0.11.3",
                "jinja2 == 2.8", 
                "itsdangerous == 0.24", 
                "markupsafe == 0.23"]
               , force_create = True)

sys.path.append(ENV_NAME + "/site-packages")

from flask import Flask, Markup
from jinja2 import Template 

app = Flask(__name__)

@app.route('/')
def test_template():
    t = Template("Hello, World!: {% for n in range(1,10) %}{{n}} " "{% endfor %}")
    return t.render()

if __name__ == '__main__':
    app.config['TESTING'] = True
    assert isinstance(app, Flask)
    server = multiprocessing.Process(target=app.run)
    server.start()
    time.sleep(1)
    f = urllib2.urlopen("http://127.0.0.1:5000/", timeout=1)
    s = f.read()
    assert 'Hello, World!: 1 2 3 4 5 6 7 8 9' in s
    server.terminate()
    server.join()
    m1 = Markup('<strong>Hello %s!</strong>') % '<blink>hacker</blink>'
    m2 = Markup.escape('<blink>hacker</blink>')
    m3 = Markup('<em>Marked up</em> &raquo; HTML').striptags()
    print 'PASSED'
