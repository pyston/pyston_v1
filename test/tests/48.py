# should_error
# This should raise a python level error, not an assertion in the compiler

x = 1
y = x.doesnt_exist
print y + 1
