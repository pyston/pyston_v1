import sys
import subprocess

me = sys.executable

p = subprocess.Popen([me, "-Sc", "1/0"], stdout=subprocess.PIPE)

print p.stdout.read()
