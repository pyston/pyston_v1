# should_error

# Expected failure: even though we could copy the "definedness" information in the assignment,
# that's actually an error.

if 0:
    x = 1
y = x
if 0:
    print y
