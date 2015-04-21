# should_error
# no-collect-stats

try:
    raise SystemExit, "hello"
except Exception as e:
    pass
