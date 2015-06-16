import termios, sys
fd = sys.stdout.fileno()
try:
    print termios.tcgetattr(fd)
except:
    print sys.exc_info()[1]
