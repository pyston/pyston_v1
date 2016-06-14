import os
import signal
import threading
import time

def f():
    time.sleep(0.1)
    os.kill(os.getpid(), signal.SIGINT)
t = threading.Thread(target=f)
t.start()

def g():
    while True:
        -(0.2 ** 5)
try:
    g()
    assert 0
except KeyboardInterrupt:
    pass
