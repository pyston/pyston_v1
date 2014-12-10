# We need to be able to parse files with exec statements in them,
# even if we don't support the actual exec statement yet.


def dont_run_me():
    exec "1/0"
