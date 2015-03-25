try:
    from . import doesnt_exist
except ValueError, e:
    print e
