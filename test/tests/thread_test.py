from thread import start_new_thread
import time

done = 0
def run(arg):
    global done
    print "in other thread!", arg
    done = 1


print "starting!"
t = start_new_thread(run, (5,))
print type(t)

while not done:
    time.sleep(0)

print "done!"

