We're currently trying out [codespeed](http://github.com/tobami/codespeed) for performance tracking.

These instructions are pretty untested.

## Server setup

```
git clone http://github.com/kmod/codespeed --branch pyston
virtualenv codespeed_env
. codespeed_env/bin/activate
cd codespeed
pip install -r requirements
python manage.py syncdb
# create admin user when prompted
python manage.py migrate
python manage.py collectstatic

cp sample_project/deploy/apache-reverseproxy.conf /etc/apache2/sites-available/010-speed.pyston.conf
ln -s /etc/apache2/sites-available/010-speed.pyston.conf /etc/apache2/sites-enabled
# may need:
# a2enmod proxy_http
# service apache2 restart
service apache2 reload
```

Create an "environment" for each computer that it will be run on.  Right now the tools are set up to set the environment to the hostname.
