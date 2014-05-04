# expected: fail
# - string iteration

# This should probably be moved into the str_functions test, once it's no longer failing

for c in "hello world":
    print repr(c)

