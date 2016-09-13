# run_args: -n
# statcheck: ("-L" in EXTRA_JIT_ARGS) or (1 <= stats["num_osr_exits"] <= 2)
# statcheck: noninit_count('slowpath_binop') <= 10

x = 100000
y = 0
while x:
    x = x - 1
    y = y + x
    # print x, y

print y

