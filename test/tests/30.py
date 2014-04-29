# run_args: -n
# statcheck: ("-O" in EXTRA_JIT_ARGS) or (1 <= stats["OSR exits"] <= 2)
# statcheck: stats['slowpath_binop'] <= 10

x = 100000
y = 0
while x:
    x = x - 1
    y = y + x
    # print x, y

print y

