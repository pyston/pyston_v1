def f(a, b, c, d, e):
  if a<b:
    if c<d:
      if a<c:
        if c<e:
          if d<e:
            if b<d:
              if b<c:
                return (a, b, c, d, e)
              else:
                return (a, c, b, d, e)
            else:
              if b<e:
                return (a, c, d, b, e)
              else:
                return (a, c, d, e, b)
          else:
            if b<e:
              if b<c:
                return (a, b, c, e, d)
              else:
                return (a, c, b, e, d)
            else:
              if b<d:
                return (a, c, e, b, d)
              else:
                return (a, c, e, d, b)
        else:
          if b<c:
            if a<e:
              if b<e:
                return (a, b, e, c, d)
              else:
                return (a, e, b, c, d)
            else:
              return (e, a, b, c, d)
          else:
            if b<d:
              if a<e:
                return (a, e, c, b, d)
              else:
                return (e, a, c, b, d)
            else:
              if a<e:
                return (a, e, c, d, b)
              else:
                return (e, a, c, d, b)
      else:
        if a<e:
          if b<e:
            if b<d:
              if d<e:
                return (c, a, b, d, e)
              else:
                return (c, a, b, e, d)
            else:
              if a<d:
                return (c, a, d, b, e)
              else:
                return (c, d, a, b, e)
          else:
            if d<e:
              if a<d:
                return (c, a, d, e, b)
              else:
                return (c, d, a, e, b)
            else:
              if b<d:
                return (c, a, e, b, d)
              else:
                return (c, a, e, d, b)
        else:
          if a<d:
            if b<d:
              if c<e:
                return (c, e, a, b, d)
              else:
                return (e, c, a, b, d)
            else:
              if c<e:
                return (c, e, a, d, b)
              else:
                return (e, c, a, d, b)
          else:
            if c<e:
              if d<e:
                return (c, d, e, a, b)
              else:
                return (c, e, d, a, b)
            else:
              return (e, c, d, a, b)
    else:
      if b<c:
        if b<e:
          if b<d:
            if c<e:
              return (a, b, d, c, e)
            else:
              if d<e:
                return (a, b, d, e, c)
              else:
                return (a, b, e, d, c)
          else:
            if a<d:
              if c<e:
                return (a, d, b, c, e)
              else:
                return (a, d, b, e, c)
            else:
              if c<e:
                return (d, a, b, c, e)
              else:
                return (d, a, b, e, c)
        else:
          if a<e:
            if d<e:
              if a<d:
                return (a, d, e, b, c)
              else:
                return (d, a, e, b, c)
            else:
              if b<d:
                return (a, e, b, d, c)
              else:
                return (a, e, d, b, c)
          else:
            if a<d:
              if b<d:
                return (e, a, b, d, c)
              else:
                return (e, a, d, b, c)
            else:
              if d<e:
                return (d, e, a, b, c)
              else:
                return (e, d, a, b, c)
      else:
        if c<e:
          if a<c:
            if a<d:
              if b<e:
                return (a, d, c, b, e)
              else:
                return (a, d, c, e, b)
            else:
              if b<e:
                return (d, a, c, b, e)
              else:
                return (d, a, c, e, b)
          else:
            if a<e:
              if b<e:
                return (d, c, a, b, e)
              else:
                return (d, c, a, e, b)
            else:
              return (d, c, e, a, b)
        else:
          if d<e:
            if a<e:
              if a<d:
                return (a, d, e, c, b)
              else:
                return (d, a, e, c, b)
            else:
              if a<c:
                return (d, e, a, c, b)
              else:
                return (d, e, c, a, b)
          else:
            if a<d:
              if a<e:
                return (a, e, d, c, b)
              else:
                return (e, a, d, c, b)
            else:
              if a<c:
                return (e, d, a, c, b)
              else:
                return (e, d, c, a, b)
  else:
    if c<d:
      if b<c:
        if c<e:
          if d<e:
            if a<d:
              if a<c:
                return (b, a, c, d, e)
              else:
                return (b, c, a, d, e)
            else:
              if a<e:
                return (b, c, d, a, e)
              else:
                return (b, c, d, e, a)
          else:
            if a<e:
              if a<c:
                return (b, a, c, e, d)
              else:
                return (b, c, a, e, d)
            else:
              if a<d:
                return (b, c, e, a, d)
              else:
                return (b, c, e, d, a)
        else:
          if a<c:
            if a<e:
              return (b, a, e, c, d)
            else:
              if b<e:
                return (b, e, a, c, d)
              else:
                return (e, b, a, c, d)
          else:
            if a<d:
              if b<e:
                return (b, e, c, a, d)
              else:
                return (e, b, c, a, d)
            else:
              if b<e:
                return (b, e, c, d, a)
              else:
                return (e, b, c, d, a)
      else:
        if b<e:
          if a<e:
            if a<d:
              if d<e:
                return (c, b, a, d, e)
              else:
                return (c, b, a, e, d)
            else:
              if b<d:
                return (c, b, d, a, e)
              else:
                return (c, d, b, a, e)
          else:
            if d<e:
              if b<d:
                return (c, b, d, e, a)
              else:
                return (c, d, b, e, a)
            else:
              if a<d:
                return (c, b, e, a, d)
              else:
                return (c, b, e, d, a)
        else:
          if b<d:
            if a<d:
              if c<e:
                return (c, e, b, a, d)
              else:
                return (e, c, b, a, d)
            else:
              if c<e:
                return (c, e, b, d, a)
              else:
                return (e, c, b, d, a)
          else:
            if c<e:
              if d<e:
                return (c, d, e, b, a)
              else:
                return (c, e, d, b, a)
            else:
              return (e, c, d, b, a)
    else:
      if a<c:
        if a<e:
          if a<d:
            if c<e:
              return (b, a, d, c, e)
            else:
              if d<e:
                return (b, a, d, e, c)
              else:
                return (b, a, e, d, c)
          else:
            if b<d:
              if c<e:
                return (b, d, a, c, e)
              else:
                return (b, d, a, e, c)
            else:
              if c<e:
                return (d, b, a, c, e)
              else:
                return (d, b, a, e, c)
        else:
          if b<e:
            if d<e:
              if b<d:
                return (b, d, e, a, c)
              else:
                return (d, b, e, a, c)
            else:
              if a<d:
                return (b, e, a, d, c)
              else:
                return (b, e, d, a, c)
          else:
            if b<d:
              if a<d:
                return (e, b, a, d, c)
              else:
                return (e, b, d, a, c)
            else:
              if d<e:
                return (d, e, b, a, c)
              else:
                return (e, d, b, a, c)
      else:
        if c<e:
          if b<c:
            if b<d:
              if a<e:
                return (b, d, c, a, e)
              else:
                return (b, d, c, e, a)
            else:
              if a<e:
                return (d, b, c, a, e)
              else:
                return (d, b, c, e, a)
          else:
            if a<e:
              return (d, c, b, a, e)
            else:
              if b<e:
                return (d, c, b, e, a)
              else:
                return (d, c, e, b, a)
        else:
          if d<e:
            if b<e:
              if b<d:
                return (b, d, e, c, a)
              else:
                return (d, b, e, c, a)
            else:
              if b<c:
                return (d, e, b, c, a)
              else:
                return (d, e, c, b, a)
          else:
            if b<d:
              if b<e:
                return (b, e, d, c, a)
              else:
                return (e, b, d, c, a)
            else:
              if b<c:
                return (e, d, b, c, a)
              else:
                return (e, d, c, b, a)

print f(2, 1, 0, 3, 4)
print f(1, 4, 3, 0, 2)
print f(2, 0, 1, 4, 3)
print f(0, 1, 4, 3, 2)
print f(4, 1, 3, 0, 2)
print f(4, 3, 0, 2, 1)
print f(4, 2, 0, 3, 1)
print f(1, 0, 3, 2, 4)
print f(0, 3, 1, 2, 4)
print f(2, 0, 3, 1, 4)
