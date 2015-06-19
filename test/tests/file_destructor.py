import gc

def open_lots_of_files():
    for x in range(0, 10000):
        f = open("/dev/null")
        if x % 80 == 0:
            gc.collect();

open_lots_of_files()
