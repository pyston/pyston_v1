try:
  import gc
  have_gc = True
except e:
  pass

def open_lots_of_files():
  for x in range(0, 5000):
    f = open("/dev/null")
    if have_gc:
      gc.collect();

open_lots_of_files()
