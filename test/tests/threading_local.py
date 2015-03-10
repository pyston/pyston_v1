import threading

a = threading.local()
a.x = "hello world"

def f():
    a.x = "goodbye world"
    print a.x

thread = threading.Thread(target=f)
thread.start()
thread.join()

print a.x
