import subprocess

subprocess.check_call(["true"])

# This constructor is usually called using keyword arguments, but we don't currently support
# keyword names on built-in functions.
p = subprocess.Popen(["echo", "hello", "world"], 0, None, None, subprocess.PIPE)
out, err = p.communicate()
print (out, err)

print p.wait()
