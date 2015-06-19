# From https://github.com/Wilfred/the_end_times
from sys import argv

try:
  N = int(argv[1])
except:
  N = 100

i = k = ns = 0
k1 = 1
n,a,d,t,u = (1,0,1,0,0)
while(1):
  k += 1
  t = n<<1
  n *= k
  a += t
  k1 += 2
  a *= k1
  d *= k1
  if a >= n:
    t,u = divmod(n*3 +a,d)
    u += n
    if d > u:
      ns = ns*10 + t
      i += 1
      if i % 10 == 0:
        print ('%010d\t:%d' % (ns, i))
        ns = 0
      if i >= N:
        break
      a -= d*t
      a *= 10
      n *= 10
