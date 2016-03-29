# fail-if: '-x' in EXTRA_JIT_ARGS
# - we don't get syntax errors through the old parser correctly

try:
    exec ";"
    print "worked?"
except SyntaxError:
    pass

