# while I think nothing requires that this works I actually found this in a library...
import subprocess
def f():
    res = subprocess.Popen(["true"], stdout = subprocess.PIPE, stderr = subprocess.STDOUT)
    stdout = res.communicate()[0]
    res.wait()
    if res.returncode is not 0:
         raise ValueError
f()
