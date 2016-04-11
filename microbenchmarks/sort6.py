def f(a, b, c, d, e, f):
  if a<b:
    if c<d:
      if a<c:
        if e<f:
          if b<f:
            if b<c:
              if b<e:
                if c<e:
                  if d<e:
                    return (a, b, c, d, e, f)
                  else:
                    if d<f:
                      return (a, b, c, e, d, f)
                    else:
                      return (a, b, c, e, f, d)
                else:
                  if c<f:
                    if d<f:
                      return (a, b, e, c, d, f)
                    else:
                      return (a, b, e, c, f, d)
                  else:
                    return (a, b, e, f, c, d)
              else:
                if a<e:
                  if c<f:
                    if d<f:
                      return (a, e, b, c, d, f)
                    else:
                      return (a, e, b, c, f, d)
                  else:
                    return (a, e, b, f, c, d)
                else:
                  if c<f:
                    if d<f:
                      return (e, a, b, c, d, f)
                    else:
                      return (e, a, b, c, f, d)
                  else:
                    return (e, a, b, f, c, d)
            else:
              if c<e:
                if b<e:
                  if d<e:
                    if b<d:
                      return (a, c, b, d, e, f)
                    else:
                      return (a, c, d, b, e, f)
                  else:
                    if d<f:
                      return (a, c, b, e, d, f)
                    else:
                      return (a, c, b, e, f, d)
                else:
                  if b<d:
                    if d<f:
                      return (a, c, e, b, d, f)
                    else:
                      return (a, c, e, b, f, d)
                  else:
                    if d<e:
                      return (a, c, d, e, b, f)
                    else:
                      return (a, c, e, d, b, f)
              else:
                if a<e:
                  if b<d:
                    if d<f:
                      return (a, e, c, b, d, f)
                    else:
                      return (a, e, c, b, f, d)
                  else:
                    return (a, e, c, d, b, f)
                else:
                  if b<d:
                    if d<f:
                      return (e, a, c, b, d, f)
                    else:
                      return (e, a, c, b, f, d)
                  else:
                    return (e, a, c, d, b, f)
          else:
            if b<d:
              if a<e:
                if c<f:
                  if c<e:
                    return (a, c, e, f, b, d)
                  else:
                    return (a, e, c, f, b, d)
                else:
                  if b<c:
                    return (a, e, f, b, c, d)
                  else:
                    return (a, e, f, c, b, d)
              else:
                if b<c:
                  if a<f:
                    return (e, a, f, b, c, d)
                  else:
                    return (e, f, a, b, c, d)
                else:
                  if a<f:
                    if c<f:
                      return (e, a, c, f, b, d)
                    else:
                      return (e, a, f, c, b, d)
                  else:
                    return (e, f, a, c, b, d)
            else:
              if a<e:
                if c<e:
                  if d<e:
                    return (a, c, d, e, f, b)
                  else:
                    if d<f:
                      return (a, c, e, d, f, b)
                    else:
                      return (a, c, e, f, d, b)
                else:
                  if c<f:
                    if d<f:
                      return (a, e, c, d, f, b)
                    else:
                      return (a, e, c, f, d, b)
                  else:
                    return (a, e, f, c, d, b)
              else:
                if c<f:
                  if d<f:
                    return (e, a, c, d, f, b)
                  else:
                    return (e, a, c, f, d, b)
                else:
                  if a<f:
                    return (e, a, f, c, d, b)
                  else:
                    return (e, f, a, c, d, b)
        else:
          if b<e:
            if b<c:
              if b<f:
                if d<e:
                  if c<f:
                    if d<f:
                      return (a, b, c, d, f, e)
                    else:
                      return (a, b, c, f, d, e)
                  else:
                    return (a, b, f, c, d, e)
                else:
                  if c<e:
                    if c<f:
                      return (a, b, c, f, e, d)
                    else:
                      return (a, b, f, c, e, d)
                  else:
                    return (a, b, f, e, c, d)
              else:
                if a<f:
                  if c<e:
                    if d<e:
                      return (a, f, b, c, d, e)
                    else:
                      return (a, f, b, c, e, d)
                  else:
                    return (a, f, b, e, c, d)
                else:
                  if c<e:
                    if d<e:
                      return (f, a, b, c, d, e)
                    else:
                      return (f, a, b, c, e, d)
                  else:
                    return (f, a, b, e, c, d)
            else:
              if c<f:
                if b<f:
                  if d<f:
                    if b<d:
                      return (a, c, b, d, f, e)
                    else:
                      return (a, c, d, b, f, e)
                  else:
                    if d<e:
                      return (a, c, b, f, d, e)
                    else:
                      return (a, c, b, f, e, d)
                else:
                  if b<d:
                    if d<e:
                      return (a, c, f, b, d, e)
                    else:
                      return (a, c, f, b, e, d)
                  else:
                    if d<f:
                      return (a, c, d, f, b, e)
                    else:
                      return (a, c, f, d, b, e)
              else:
                if a<f:
                  if b<d:
                    if d<e:
                      return (a, f, c, b, d, e)
                    else:
                      return (a, f, c, b, e, d)
                  else:
                    return (a, f, c, d, b, e)
                else:
                  if b<d:
                    if d<e:
                      return (f, a, c, b, d, e)
                    else:
                      return (f, a, c, b, e, d)
                  else:
                    return (f, a, c, d, b, e)
          else:
            if b<d:
              if a<f:
                if c<e:
                  if c<f:
                    return (a, c, f, e, b, d)
                  else:
                    return (a, f, c, e, b, d)
                else:
                  if b<c:
                    return (a, f, e, b, c, d)
                  else:
                    return (a, f, e, c, b, d)
              else:
                if b<c:
                  if a<e:
                    return (f, a, e, b, c, d)
                  else:
                    return (f, e, a, b, c, d)
                else:
                  if a<e:
                    if c<e:
                      return (f, a, c, e, b, d)
                    else:
                      return (f, a, e, c, b, d)
                  else:
                    return (f, e, a, c, b, d)
            else:
              if d<e:
                if c<f:
                  if d<f:
                    return (a, c, d, f, e, b)
                  else:
                    return (a, c, f, d, e, b)
                else:
                  if a<f:
                    return (a, f, c, d, e, b)
                  else:
                    return (f, a, c, d, e, b)
              else:
                if c<e:
                  if a<f:
                    if c<f:
                      return (a, c, f, e, d, b)
                    else:
                      return (a, f, c, e, d, b)
                  else:
                    return (f, a, c, e, d, b)
                else:
                  if a<e:
                    if a<f:
                      return (a, f, e, c, d, b)
                    else:
                      return (f, a, e, c, d, b)
                  else:
                    return (f, e, a, c, d, b)
      else:
        if e<f:
          if b<f:
            if b<d:
              if a<e:
                if b<e:
                  if d<e:
                    return (c, a, b, d, e, f)
                  else:
                    if d<f:
                      return (c, a, b, e, d, f)
                    else:
                      return (c, a, b, e, f, d)
                else:
                  if d<f:
                    return (c, a, e, b, d, f)
                  else:
                    return (c, a, e, b, f, d)
              else:
                if c<e:
                  if d<f:
                    return (c, e, a, b, d, f)
                  else:
                    return (c, e, a, b, f, d)
                else:
                  if d<f:
                    return (e, c, a, b, d, f)
                  else:
                    return (e, c, a, b, f, d)
            else:
              if a<d:
                if a<e:
                  if b<e:
                    return (c, a, d, b, e, f)
                  else:
                    if d<e:
                      return (c, a, d, e, b, f)
                    else:
                      return (c, a, e, d, b, f)
                else:
                  if c<e:
                    return (c, e, a, d, b, f)
                  else:
                    return (e, c, a, d, b, f)
              else:
                if a<e:
                  if b<e:
                    return (c, d, a, b, e, f)
                  else:
                    return (c, d, a, e, b, f)
                else:
                  if c<e:
                    if d<e:
                      return (c, d, e, a, b, f)
                    else:
                      return (c, e, d, a, b, f)
                  else:
                    return (e, c, d, a, b, f)
          else:
            if a<f:
              if d<f:
                if a<d:
                  if a<e:
                    if d<e:
                      return (c, a, d, e, f, b)
                    else:
                      return (c, a, e, d, f, b)
                  else:
                    if c<e:
                      return (c, e, a, d, f, b)
                    else:
                      return (e, c, a, d, f, b)
                else:
                  if d<e:
                    if a<e:
                      return (c, d, a, e, f, b)
                    else:
                      return (c, d, e, a, f, b)
                  else:
                    if c<e:
                      return (c, e, d, a, f, b)
                    else:
                      return (e, c, d, a, f, b)
              else:
                if b<d:
                  if a<e:
                    return (c, a, e, f, b, d)
                  else:
                    if c<e:
                      return (c, e, a, f, b, d)
                    else:
                      return (e, c, a, f, b, d)
                else:
                  if a<e:
                    return (c, a, e, f, d, b)
                  else:
                    if c<e:
                      return (c, e, a, f, d, b)
                    else:
                      return (e, c, a, f, d, b)
            else:
              if a<d:
                if b<d:
                  if c<e:
                    return (c, e, f, a, b, d)
                  else:
                    if c<f:
                      return (e, c, f, a, b, d)
                    else:
                      return (e, f, c, a, b, d)
                else:
                  if c<e:
                    return (c, e, f, a, d, b)
                  else:
                    if c<f:
                      return (e, c, f, a, d, b)
                    else:
                      return (e, f, c, a, d, b)
              else:
                if c<e:
                  if d<e:
                    return (c, d, e, f, a, b)
                  else:
                    if d<f:
                      return (c, e, d, f, a, b)
                    else:
                      return (c, e, f, d, a, b)
                else:
                  if c<f:
                    if d<f:
                      return (e, c, d, f, a, b)
                    else:
                      return (e, c, f, d, a, b)
                  else:
                    return (e, f, c, d, a, b)
        else:
          if b<e:
            if b<d:
              if d<e:
                if a<f:
                  if b<f:
                    if d<f:
                      return (c, a, b, d, f, e)
                    else:
                      return (c, a, b, f, d, e)
                  else:
                    return (c, a, f, b, d, e)
                else:
                  if c<f:
                    return (c, f, a, b, d, e)
                  else:
                    return (f, c, a, b, d, e)
              else:
                if a<f:
                  if b<f:
                    return (c, a, b, f, e, d)
                  else:
                    return (c, a, f, b, e, d)
                else:
                  if c<f:
                    return (c, f, a, b, e, d)
                  else:
                    return (f, c, a, b, e, d)
            else:
              if a<d:
                if a<f:
                  if b<f:
                    return (c, a, d, b, f, e)
                  else:
                    if d<f:
                      return (c, a, d, f, b, e)
                    else:
                      return (c, a, f, d, b, e)
                else:
                  if c<f:
                    return (c, f, a, d, b, e)
                  else:
                    return (f, c, a, d, b, e)
              else:
                if a<f:
                  if b<f:
                    return (c, d, a, b, f, e)
                  else:
                    return (c, d, a, f, b, e)
                else:
                  if c<f:
                    if d<f:
                      return (c, d, f, a, b, e)
                    else:
                      return (c, f, d, a, b, e)
                  else:
                    return (f, c, d, a, b, e)
          else:
            if a<e:
              if d<e:
                if a<d:
                  if a<f:
                    if d<f:
                      return (c, a, d, f, e, b)
                    else:
                      return (c, a, f, d, e, b)
                  else:
                    if c<f:
                      return (c, f, a, d, e, b)
                    else:
                      return (f, c, a, d, e, b)
                else:
                  if d<f:
                    if a<f:
                      return (c, d, a, f, e, b)
                    else:
                      return (c, d, f, a, e, b)
                  else:
                    if c<f:
                      return (c, f, d, a, e, b)
                    else:
                      return (f, c, d, a, e, b)
              else:
                if b<d:
                  if a<f:
                    return (c, a, f, e, b, d)
                  else:
                    if c<f:
                      return (c, f, a, e, b, d)
                    else:
                      return (f, c, a, e, b, d)
                else:
                  if a<f:
                    return (c, a, f, e, d, b)
                  else:
                    if c<f:
                      return (c, f, a, e, d, b)
                    else:
                      return (f, c, a, e, d, b)
            else:
              if a<d:
                if b<d:
                  if c<e:
                    if c<f:
                      return (c, f, e, a, b, d)
                    else:
                      return (f, c, e, a, b, d)
                  else:
                    return (f, e, c, a, b, d)
                else:
                  if c<e:
                    if c<f:
                      return (c, f, e, a, d, b)
                    else:
                      return (f, c, e, a, d, b)
                  else:
                    return (f, e, c, a, d, b)
              else:
                if d<e:
                  if c<f:
                    if d<f:
                      return (c, d, f, e, a, b)
                    else:
                      return (c, f, d, e, a, b)
                  else:
                    return (f, c, d, e, a, b)
                else:
                  if c<e:
                    if c<f:
                      return (c, f, e, d, a, b)
                    else:
                      return (f, c, e, d, a, b)
                  else:
                    return (f, e, c, d, a, b)
    else:
      if b<c:
        if e<f:
          if a<e:
            if b<e:
              if b<d:
                if d<e:
                  if c<e:
                    return (a, b, d, c, e, f)
                  else:
                    if c<f:
                      return (a, b, d, e, c, f)
                    else:
                      return (a, b, d, e, f, c)
                else:
                  if c<f:
                    return (a, b, e, d, c, f)
                  else:
                    if d<f:
                      return (a, b, e, d, f, c)
                    else:
                      return (a, b, e, f, d, c)
              else:
                if a<d:
                  if c<e:
                    return (a, d, b, c, e, f)
                  else:
                    if c<f:
                      return (a, d, b, e, c, f)
                    else:
                      return (a, d, b, e, f, c)
                else:
                  if c<e:
                    return (d, a, b, c, e, f)
                  else:
                    if c<f:
                      return (d, a, b, e, c, f)
                    else:
                      return (d, a, b, e, f, c)
            else:
              if d<e:
                if a<d:
                  if b<f:
                    if c<f:
                      return (a, d, e, b, c, f)
                    else:
                      return (a, d, e, b, f, c)
                  else:
                    return (a, d, e, f, b, c)
                else:
                  if b<f:
                    if c<f:
                      return (d, a, e, b, c, f)
                    else:
                      return (d, a, e, b, f, c)
                  else:
                    return (d, a, e, f, b, c)
              else:
                if b<d:
                  if d<f:
                    if c<f:
                      return (a, e, b, d, c, f)
                    else:
                      return (a, e, b, d, f, c)
                  else:
                    if b<f:
                      return (a, e, b, f, d, c)
                    else:
                      return (a, e, f, b, d, c)
                else:
                  if b<f:
                    if c<f:
                      return (a, e, d, b, c, f)
                    else:
                      return (a, e, d, b, f, c)
                  else:
                    if d<f:
                      return (a, e, d, f, b, c)
                    else:
                      return (a, e, f, d, b, c)
          else:
            if a<d:
              if b<d:
                if b<f:
                  if c<f:
                    return (e, a, b, d, c, f)
                  else:
                    if d<f:
                      return (e, a, b, d, f, c)
                    else:
                      return (e, a, b, f, d, c)
                else:
                  if a<f:
                    return (e, a, f, b, d, c)
                  else:
                    return (e, f, a, b, d, c)
              else:
                if b<f:
                  if c<f:
                    return (e, a, d, b, c, f)
                  else:
                    return (e, a, d, b, f, c)
                else:
                  if a<f:
                    if d<f:
                      return (e, a, d, f, b, c)
                    else:
                      return (e, a, f, d, b, c)
                  else:
                    return (e, f, a, d, b, c)
            else:
              if d<e:
                if b<f:
                  if c<f:
                    return (d, e, a, b, c, f)
                  else:
                    return (d, e, a, b, f, c)
                else:
                  if a<f:
                    return (d, e, a, f, b, c)
                  else:
                    return (d, e, f, a, b, c)
              else:
                if a<f:
                  if b<f:
                    if c<f:
                      return (e, d, a, b, c, f)
                    else:
                      return (e, d, a, b, f, c)
                  else:
                    return (e, d, a, f, b, c)
                else:
                  if d<f:
                    return (e, d, f, a, b, c)
                  else:
                    return (e, f, d, a, b, c)
        else:
          if a<f:
            if b<f:
              if b<d:
                if c<e:
                  if c<f:
                    return (a, b, d, c, f, e)
                  else:
                    if d<f:
                      return (a, b, d, f, c, e)
                    else:
                      return (a, b, f, d, c, e)
                else:
                  if d<e:
                    if d<f:
                      return (a, b, d, f, e, c)
                    else:
                      return (a, b, f, d, e, c)
                  else:
                    return (a, b, f, e, d, c)
              else:
                if a<d:
                  if c<e:
                    if c<f:
                      return (a, d, b, c, f, e)
                    else:
                      return (a, d, b, f, c, e)
                  else:
                    return (a, d, b, f, e, c)
                else:
                  if c<e:
                    if c<f:
                      return (d, a, b, c, f, e)
                    else:
                      return (d, a, b, f, c, e)
                  else:
                    return (d, a, b, f, e, c)
            else:
              if d<f:
                if a<d:
                  if b<e:
                    if c<e:
                      return (a, d, f, b, c, e)
                    else:
                      return (a, d, f, b, e, c)
                  else:
                    return (a, d, f, e, b, c)
                else:
                  if b<e:
                    if c<e:
                      return (d, a, f, b, c, e)
                    else:
                      return (d, a, f, b, e, c)
                  else:
                    return (d, a, f, e, b, c)
              else:
                if b<d:
                  if d<e:
                    if c<e:
                      return (a, f, b, d, c, e)
                    else:
                      return (a, f, b, d, e, c)
                  else:
                    if b<e:
                      return (a, f, b, e, d, c)
                    else:
                      return (a, f, e, b, d, c)
                else:
                  if b<e:
                    if c<e:
                      return (a, f, d, b, c, e)
                    else:
                      return (a, f, d, b, e, c)
                  else:
                    if d<e:
                      return (a, f, d, e, b, c)
                    else:
                      return (a, f, e, d, b, c)
          else:
            if a<d:
              if b<d:
                if b<e:
                  if c<e:
                    return (f, a, b, d, c, e)
                  else:
                    if d<e:
                      return (f, a, b, d, e, c)
                    else:
                      return (f, a, b, e, d, c)
                else:
                  if a<e:
                    return (f, a, e, b, d, c)
                  else:
                    return (f, e, a, b, d, c)
              else:
                if b<e:
                  if c<e:
                    return (f, a, d, b, c, e)
                  else:
                    return (f, a, d, b, e, c)
                else:
                  if a<e:
                    if d<e:
                      return (f, a, d, e, b, c)
                    else:
                      return (f, a, e, d, b, c)
                  else:
                    return (f, e, a, d, b, c)
            else:
              if b<e:
                if c<e:
                  if d<f:
                    return (d, f, a, b, c, e)
                  else:
                    return (f, d, a, b, c, e)
                else:
                  if d<f:
                    return (d, f, a, b, e, c)
                  else:
                    return (f, d, a, b, e, c)
              else:
                if a<e:
                  if d<f:
                    return (d, f, a, e, b, c)
                  else:
                    return (f, d, a, e, b, c)
                else:
                  if d<e:
                    if d<f:
                      return (d, f, e, a, b, c)
                    else:
                      return (f, d, e, a, b, c)
                  else:
                    return (f, e, d, a, b, c)
      else:
        if e<f:
          if a<e:
            if a<d:
              if d<e:
                if c<e:
                  if b<e:
                    return (a, d, c, b, e, f)
                  else:
                    if b<f:
                      return (a, d, c, e, b, f)
                    else:
                      return (a, d, c, e, f, b)
                else:
                  if b<f:
                    return (a, d, e, c, b, f)
                  else:
                    if c<f:
                      return (a, d, e, c, f, b)
                    else:
                      return (a, d, e, f, c, b)
              else:
                if c<f:
                  if b<f:
                    return (a, e, d, c, b, f)
                  else:
                    return (a, e, d, c, f, b)
                else:
                  if d<f:
                    return (a, e, d, f, c, b)
                  else:
                    return (a, e, f, d, c, b)
            else:
              if b<f:
                if a<c:
                  if b<e:
                    return (d, a, c, b, e, f)
                  else:
                    if c<e:
                      return (d, a, c, e, b, f)
                    else:
                      return (d, a, e, c, b, f)
                else:
                  if b<e:
                    return (d, c, a, b, e, f)
                  else:
                    return (d, c, a, e, b, f)
              else:
                if c<e:
                  if a<c:
                    return (d, a, c, e, f, b)
                  else:
                    return (d, c, a, e, f, b)
                else:
                  if c<f:
                    return (d, a, e, c, f, b)
                  else:
                    return (d, a, e, f, c, b)
          else:
            if a<c:
              if c<f:
                if b<f:
                  if a<d:
                    return (e, a, d, c, b, f)
                  else:
                    if d<e:
                      return (d, e, a, c, b, f)
                    else:
                      return (e, d, a, c, b, f)
                else:
                  if a<d:
                    return (e, a, d, c, f, b)
                  else:
                    if d<e:
                      return (d, e, a, c, f, b)
                    else:
                      return (e, d, a, c, f, b)
              else:
                if a<f:
                  if a<d:
                    if d<f:
                      return (e, a, d, f, c, b)
                    else:
                      return (e, a, f, d, c, b)
                  else:
                    if d<e:
                      return (d, e, a, f, c, b)
                    else:
                      return (e, d, a, f, c, b)
                else:
                  if d<f:
                    if d<e:
                      return (d, e, f, a, c, b)
                    else:
                      return (e, d, f, a, c, b)
                  else:
                    if a<d:
                      return (e, f, a, d, c, b)
                    else:
                      return (e, f, d, a, c, b)
            else:
              if a<f:
                if b<f:
                  if c<e:
                    return (d, c, e, a, b, f)
                  else:
                    if d<e:
                      return (d, e, c, a, b, f)
                    else:
                      return (e, d, c, a, b, f)
                else:
                  if c<e:
                    return (d, c, e, a, f, b)
                  else:
                    if d<e:
                      return (d, e, c, a, f, b)
                    else:
                      return (e, d, c, a, f, b)
              else:
                if d<e:
                  if c<e:
                    return (d, c, e, f, a, b)
                  else:
                    if c<f:
                      return (d, e, c, f, a, b)
                    else:
                      return (d, e, f, c, a, b)
                else:
                  if c<f:
                    return (e, d, c, f, a, b)
                  else:
                    if d<f:
                      return (e, d, f, c, a, b)
                    else:
                      return (e, f, d, c, a, b)
        else:
          if a<f:
            if a<d:
              if b<e:
                if c<f:
                  if b<f:
                    return (a, d, c, b, f, e)
                  else:
                    return (a, d, c, f, b, e)
                else:
                  if d<f:
                    return (a, d, f, c, b, e)
                  else:
                    return (a, f, d, c, b, e)
              else:
                if c<e:
                  if c<f:
                    return (a, d, c, f, e, b)
                  else:
                    if d<f:
                      return (a, d, f, c, e, b)
                    else:
                      return (a, f, d, c, e, b)
                else:
                  if d<e:
                    if d<f:
                      return (a, d, f, e, c, b)
                    else:
                      return (a, f, d, e, c, b)
                  else:
                    return (a, f, e, d, c, b)
            else:
              if b<e:
                if a<c:
                  if b<f:
                    return (d, a, c, b, f, e)
                  else:
                    if c<f:
                      return (d, a, c, f, b, e)
                    else:
                      return (d, a, f, c, b, e)
                else:
                  if b<f:
                    return (d, c, a, b, f, e)
                  else:
                    return (d, c, a, f, b, e)
              else:
                if c<f:
                  if a<c:
                    return (d, a, c, f, e, b)
                  else:
                    return (d, c, a, f, e, b)
                else:
                  if c<e:
                    return (d, a, f, c, e, b)
                  else:
                    return (d, a, f, e, c, b)
          else:
            if a<c:
              if c<e:
                if b<e:
                  if a<d:
                    return (f, a, d, c, b, e)
                  else:
                    if d<f:
                      return (d, f, a, c, b, e)
                    else:
                      return (f, d, a, c, b, e)
                else:
                  if a<d:
                    return (f, a, d, c, e, b)
                  else:
                    if d<f:
                      return (d, f, a, c, e, b)
                    else:
                      return (f, d, a, c, e, b)
              else:
                if a<e:
                  if a<d:
                    if d<e:
                      return (f, a, d, e, c, b)
                    else:
                      return (f, a, e, d, c, b)
                  else:
                    if d<f:
                      return (d, f, a, e, c, b)
                    else:
                      return (f, d, a, e, c, b)
                else:
                  if d<e:
                    if d<f:
                      return (d, f, e, a, c, b)
                    else:
                      return (f, d, e, a, c, b)
                  else:
                    if a<d:
                      return (f, e, a, d, c, b)
                    else:
                      return (f, e, d, a, c, b)
            else:
              if a<e:
                if b<e:
                  if c<f:
                    return (d, c, f, a, b, e)
                  else:
                    if d<f:
                      return (d, f, c, a, b, e)
                    else:
                      return (f, d, c, a, b, e)
                else:
                  if c<f:
                    return (d, c, f, a, e, b)
                  else:
                    if d<f:
                      return (d, f, c, a, e, b)
                    else:
                      return (f, d, c, a, e, b)
              else:
                if c<e:
                  if c<f:
                    return (d, c, f, e, a, b)
                  else:
                    if d<f:
                      return (d, f, c, e, a, b)
                    else:
                      return (f, d, c, e, a, b)
                else:
                  if d<e:
                    if d<f:
                      return (d, f, e, c, a, b)
                    else:
                      return (f, d, e, c, a, b)
                  else:
                    return (f, e, d, c, a, b)
  else:
    if c<d:
      if b<c:
        if e<f:
          if a<f:
            if a<c:
              if a<e:
                if c<e:
                  if d<e:
                    return (b, a, c, d, e, f)
                  else:
                    if d<f:
                      return (b, a, c, e, d, f)
                    else:
                      return (b, a, c, e, f, d)
                else:
                  if c<f:
                    if d<f:
                      return (b, a, e, c, d, f)
                    else:
                      return (b, a, e, c, f, d)
                  else:
                    return (b, a, e, f, c, d)
              else:
                if b<e:
                  if c<f:
                    if d<f:
                      return (b, e, a, c, d, f)
                    else:
                      return (b, e, a, c, f, d)
                  else:
                    return (b, e, a, f, c, d)
                else:
                  if c<f:
                    if d<f:
                      return (e, b, a, c, d, f)
                    else:
                      return (e, b, a, c, f, d)
                  else:
                    return (e, b, a, f, c, d)
            else:
              if c<e:
                if a<e:
                  if d<e:
                    if a<d:
                      return (b, c, a, d, e, f)
                    else:
                      return (b, c, d, a, e, f)
                  else:
                    if d<f:
                      return (b, c, a, e, d, f)
                    else:
                      return (b, c, a, e, f, d)
                else:
                  if a<d:
                    if d<f:
                      return (b, c, e, a, d, f)
                    else:
                      return (b, c, e, a, f, d)
                  else:
                    if d<e:
                      return (b, c, d, e, a, f)
                    else:
                      return (b, c, e, d, a, f)
              else:
                if b<e:
                  if a<d:
                    if d<f:
                      return (b, e, c, a, d, f)
                    else:
                      return (b, e, c, a, f, d)
                  else:
                    return (b, e, c, d, a, f)
                else:
                  if a<d:
                    if d<f:
                      return (e, b, c, a, d, f)
                    else:
                      return (e, b, c, a, f, d)
                  else:
                    return (e, b, c, d, a, f)
          else:
            if a<d:
              if b<e:
                if c<f:
                  if c<e:
                    return (b, c, e, f, a, d)
                  else:
                    return (b, e, c, f, a, d)
                else:
                  if a<c:
                    return (b, e, f, a, c, d)
                  else:
                    return (b, e, f, c, a, d)
              else:
                if a<c:
                  if b<f:
                    return (e, b, f, a, c, d)
                  else:
                    return (e, f, b, a, c, d)
                else:
                  if b<f:
                    if c<f:
                      return (e, b, c, f, a, d)
                    else:
                      return (e, b, f, c, a, d)
                  else:
                    return (e, f, b, c, a, d)
            else:
              if b<e:
                if c<e:
                  if d<e:
                    return (b, c, d, e, f, a)
                  else:
                    if d<f:
                      return (b, c, e, d, f, a)
                    else:
                      return (b, c, e, f, d, a)
                else:
                  if c<f:
                    if d<f:
                      return (b, e, c, d, f, a)
                    else:
                      return (b, e, c, f, d, a)
                  else:
                    return (b, e, f, c, d, a)
              else:
                if c<f:
                  if d<f:
                    return (e, b, c, d, f, a)
                  else:
                    return (e, b, c, f, d, a)
                else:
                  if b<f:
                    return (e, b, f, c, d, a)
                  else:
                    return (e, f, b, c, d, a)
        else:
          if a<e:
            if a<c:
              if a<f:
                if d<e:
                  if c<f:
                    if d<f:
                      return (b, a, c, d, f, e)
                    else:
                      return (b, a, c, f, d, e)
                  else:
                    return (b, a, f, c, d, e)
                else:
                  if c<e:
                    if c<f:
                      return (b, a, c, f, e, d)
                    else:
                      return (b, a, f, c, e, d)
                  else:
                    return (b, a, f, e, c, d)
              else:
                if b<f:
                  if c<e:
                    if d<e:
                      return (b, f, a, c, d, e)
                    else:
                      return (b, f, a, c, e, d)
                  else:
                    return (b, f, a, e, c, d)
                else:
                  if c<e:
                    if d<e:
                      return (f, b, a, c, d, e)
                    else:
                      return (f, b, a, c, e, d)
                  else:
                    return (f, b, a, e, c, d)
            else:
              if c<f:
                if a<f:
                  if d<f:
                    if a<d:
                      return (b, c, a, d, f, e)
                    else:
                      return (b, c, d, a, f, e)
                  else:
                    if d<e:
                      return (b, c, a, f, d, e)
                    else:
                      return (b, c, a, f, e, d)
                else:
                  if a<d:
                    if d<e:
                      return (b, c, f, a, d, e)
                    else:
                      return (b, c, f, a, e, d)
                  else:
                    if d<f:
                      return (b, c, d, f, a, e)
                    else:
                      return (b, c, f, d, a, e)
              else:
                if b<f:
                  if a<d:
                    if d<e:
                      return (b, f, c, a, d, e)
                    else:
                      return (b, f, c, a, e, d)
                  else:
                    return (b, f, c, d, a, e)
                else:
                  if a<d:
                    if d<e:
                      return (f, b, c, a, d, e)
                    else:
                      return (f, b, c, a, e, d)
                  else:
                    return (f, b, c, d, a, e)
          else:
            if a<d:
              if b<f:
                if c<e:
                  if c<f:
                    return (b, c, f, e, a, d)
                  else:
                    return (b, f, c, e, a, d)
                else:
                  if a<c:
                    return (b, f, e, a, c, d)
                  else:
                    return (b, f, e, c, a, d)
              else:
                if a<c:
                  if b<e:
                    return (f, b, e, a, c, d)
                  else:
                    return (f, e, b, a, c, d)
                else:
                  if b<e:
                    if c<e:
                      return (f, b, c, e, a, d)
                    else:
                      return (f, b, e, c, a, d)
                  else:
                    return (f, e, b, c, a, d)
            else:
              if d<e:
                if c<f:
                  if d<f:
                    return (b, c, d, f, e, a)
                  else:
                    return (b, c, f, d, e, a)
                else:
                  if b<f:
                    return (b, f, c, d, e, a)
                  else:
                    return (f, b, c, d, e, a)
              else:
                if c<e:
                  if b<f:
                    if c<f:
                      return (b, c, f, e, d, a)
                    else:
                      return (b, f, c, e, d, a)
                  else:
                    return (f, b, c, e, d, a)
                else:
                  if b<e:
                    if b<f:
                      return (b, f, e, c, d, a)
                    else:
                      return (f, b, e, c, d, a)
                  else:
                    return (f, e, b, c, d, a)
      else:
        if e<f:
          if a<f:
            if a<d:
              if b<e:
                if a<e:
                  if d<e:
                    return (c, b, a, d, e, f)
                  else:
                    if d<f:
                      return (c, b, a, e, d, f)
                    else:
                      return (c, b, a, e, f, d)
                else:
                  if d<f:
                    return (c, b, e, a, d, f)
                  else:
                    return (c, b, e, a, f, d)
              else:
                if c<e:
                  if d<f:
                    return (c, e, b, a, d, f)
                  else:
                    return (c, e, b, a, f, d)
                else:
                  if d<f:
                    return (e, c, b, a, d, f)
                  else:
                    return (e, c, b, a, f, d)
            else:
              if b<d:
                if b<e:
                  if a<e:
                    return (c, b, d, a, e, f)
                  else:
                    if d<e:
                      return (c, b, d, e, a, f)
                    else:
                      return (c, b, e, d, a, f)
                else:
                  if c<e:
                    return (c, e, b, d, a, f)
                  else:
                    return (e, c, b, d, a, f)
              else:
                if b<e:
                  if a<e:
                    return (c, d, b, a, e, f)
                  else:
                    return (c, d, b, e, a, f)
                else:
                  if c<e:
                    if d<e:
                      return (c, d, e, b, a, f)
                    else:
                      return (c, e, d, b, a, f)
                  else:
                    return (e, c, d, b, a, f)
          else:
            if b<f:
              if d<f:
                if b<d:
                  if b<e:
                    if d<e:
                      return (c, b, d, e, f, a)
                    else:
                      return (c, b, e, d, f, a)
                  else:
                    if c<e:
                      return (c, e, b, d, f, a)
                    else:
                      return (e, c, b, d, f, a)
                else:
                  if d<e:
                    if b<e:
                      return (c, d, b, e, f, a)
                    else:
                      return (c, d, e, b, f, a)
                  else:
                    if c<e:
                      return (c, e, d, b, f, a)
                    else:
                      return (e, c, d, b, f, a)
              else:
                if a<d:
                  if b<e:
                    return (c, b, e, f, a, d)
                  else:
                    if c<e:
                      return (c, e, b, f, a, d)
                    else:
                      return (e, c, b, f, a, d)
                else:
                  if b<e:
                    return (c, b, e, f, d, a)
                  else:
                    if c<e:
                      return (c, e, b, f, d, a)
                    else:
                      return (e, c, b, f, d, a)
            else:
              if b<d:
                if a<d:
                  if c<e:
                    return (c, e, f, b, a, d)
                  else:
                    if c<f:
                      return (e, c, f, b, a, d)
                    else:
                      return (e, f, c, b, a, d)
                else:
                  if c<e:
                    return (c, e, f, b, d, a)
                  else:
                    if c<f:
                      return (e, c, f, b, d, a)
                    else:
                      return (e, f, c, b, d, a)
              else:
                if c<e:
                  if d<e:
                    return (c, d, e, f, b, a)
                  else:
                    if d<f:
                      return (c, e, d, f, b, a)
                    else:
                      return (c, e, f, d, b, a)
                else:
                  if c<f:
                    if d<f:
                      return (e, c, d, f, b, a)
                    else:
                      return (e, c, f, d, b, a)
                  else:
                    return (e, f, c, d, b, a)
        else:
          if a<e:
            if a<d:
              if d<e:
                if a<f:
                  if d<f:
                    return (c, b, a, d, f, e)
                  else:
                    return (c, b, a, f, d, e)
                else:
                  if b<f:
                    return (c, b, f, a, d, e)
                  else:
                    if c<f:
                      return (c, f, b, a, d, e)
                    else:
                      return (f, c, b, a, d, e)
              else:
                if b<f:
                  if a<f:
                    return (c, b, a, f, e, d)
                  else:
                    return (c, b, f, a, e, d)
                else:
                  if c<f:
                    return (c, f, b, a, e, d)
                  else:
                    return (f, c, b, a, e, d)
            else:
              if b<d:
                if b<f:
                  if a<f:
                    return (c, b, d, a, f, e)
                  else:
                    if d<f:
                      return (c, b, d, f, a, e)
                    else:
                      return (c, b, f, d, a, e)
                else:
                  if c<f:
                    return (c, f, b, d, a, e)
                  else:
                    return (f, c, b, d, a, e)
              else:
                if b<f:
                  if a<f:
                    return (c, d, b, a, f, e)
                  else:
                    return (c, d, b, f, a, e)
                else:
                  if c<f:
                    if d<f:
                      return (c, d, f, b, a, e)
                    else:
                      return (c, f, d, b, a, e)
                  else:
                    return (f, c, d, b, a, e)
          else:
            if b<e:
              if d<e:
                if b<d:
                  if b<f:
                    if d<f:
                      return (c, b, d, f, e, a)
                    else:
                      return (c, b, f, d, e, a)
                  else:
                    if c<f:
                      return (c, f, b, d, e, a)
                    else:
                      return (f, c, b, d, e, a)
                else:
                  if d<f:
                    if b<f:
                      return (c, d, b, f, e, a)
                    else:
                      return (c, d, f, b, e, a)
                  else:
                    if c<f:
                      return (c, f, d, b, e, a)
                    else:
                      return (f, c, d, b, e, a)
              else:
                if a<d:
                  if b<f:
                    return (c, b, f, e, a, d)
                  else:
                    if c<f:
                      return (c, f, b, e, a, d)
                    else:
                      return (f, c, b, e, a, d)
                else:
                  if b<f:
                    return (c, b, f, e, d, a)
                  else:
                    if c<f:
                      return (c, f, b, e, d, a)
                    else:
                      return (f, c, b, e, d, a)
            else:
              if b<d:
                if a<d:
                  if c<e:
                    if c<f:
                      return (c, f, e, b, a, d)
                    else:
                      return (f, c, e, b, a, d)
                  else:
                    return (f, e, c, b, a, d)
                else:
                  if c<e:
                    if c<f:
                      return (c, f, e, b, d, a)
                    else:
                      return (f, c, e, b, d, a)
                  else:
                    return (f, e, c, b, d, a)
              else:
                if d<e:
                  if c<f:
                    if d<f:
                      return (c, d, f, e, b, a)
                    else:
                      return (c, f, d, e, b, a)
                  else:
                    return (f, c, d, e, b, a)
                else:
                  if c<e:
                    if c<f:
                      return (c, f, e, d, b, a)
                    else:
                      return (f, c, e, d, b, a)
                  else:
                    return (f, e, c, d, b, a)
    else:
      if a<c:
        if e<f:
          if b<e:
            if a<e:
              if a<d:
                if d<e:
                  if c<e:
                    return (b, a, d, c, e, f)
                  else:
                    if c<f:
                      return (b, a, d, e, c, f)
                    else:
                      return (b, a, d, e, f, c)
                else:
                  if c<f:
                    return (b, a, e, d, c, f)
                  else:
                    if d<f:
                      return (b, a, e, d, f, c)
                    else:
                      return (b, a, e, f, d, c)
              else:
                if b<d:
                  if c<e:
                    return (b, d, a, c, e, f)
                  else:
                    if c<f:
                      return (b, d, a, e, c, f)
                    else:
                      return (b, d, a, e, f, c)
                else:
                  if c<e:
                    return (d, b, a, c, e, f)
                  else:
                    if c<f:
                      return (d, b, a, e, c, f)
                    else:
                      return (d, b, a, e, f, c)
            else:
              if d<e:
                if b<d:
                  if a<f:
                    if c<f:
                      return (b, d, e, a, c, f)
                    else:
                      return (b, d, e, a, f, c)
                  else:
                    return (b, d, e, f, a, c)
                else:
                  if a<f:
                    if c<f:
                      return (d, b, e, a, c, f)
                    else:
                      return (d, b, e, a, f, c)
                  else:
                    return (d, b, e, f, a, c)
              else:
                if a<d:
                  if d<f:
                    if c<f:
                      return (b, e, a, d, c, f)
                    else:
                      return (b, e, a, d, f, c)
                  else:
                    if a<f:
                      return (b, e, a, f, d, c)
                    else:
                      return (b, e, f, a, d, c)
                else:
                  if a<f:
                    if c<f:
                      return (b, e, d, a, c, f)
                    else:
                      return (b, e, d, a, f, c)
                  else:
                    if d<f:
                      return (b, e, d, f, a, c)
                    else:
                      return (b, e, f, d, a, c)
          else:
            if b<d:
              if a<d:
                if a<f:
                  if c<f:
                    return (e, b, a, d, c, f)
                  else:
                    if d<f:
                      return (e, b, a, d, f, c)
                    else:
                      return (e, b, a, f, d, c)
                else:
                  if b<f:
                    return (e, b, f, a, d, c)
                  else:
                    return (e, f, b, a, d, c)
              else:
                if a<f:
                  if c<f:
                    return (e, b, d, a, c, f)
                  else:
                    return (e, b, d, a, f, c)
                else:
                  if b<f:
                    if d<f:
                      return (e, b, d, f, a, c)
                    else:
                      return (e, b, f, d, a, c)
                  else:
                    return (e, f, b, d, a, c)
            else:
              if d<e:
                if a<f:
                  if c<f:
                    return (d, e, b, a, c, f)
                  else:
                    return (d, e, b, a, f, c)
                else:
                  if b<f:
                    return (d, e, b, f, a, c)
                  else:
                    return (d, e, f, b, a, c)
              else:
                if a<f:
                  if c<f:
                    return (e, d, b, a, c, f)
                  else:
                    return (e, d, b, a, f, c)
                else:
                  if b<f:
                    return (e, d, b, f, a, c)
                  else:
                    if d<f:
                      return (e, d, f, b, a, c)
                    else:
                      return (e, f, d, b, a, c)
        else:
          if b<f:
            if a<f:
              if a<d:
                if c<e:
                  if c<f:
                    return (b, a, d, c, f, e)
                  else:
                    if d<f:
                      return (b, a, d, f, c, e)
                    else:
                      return (b, a, f, d, c, e)
                else:
                  if d<e:
                    if d<f:
                      return (b, a, d, f, e, c)
                    else:
                      return (b, a, f, d, e, c)
                  else:
                    return (b, a, f, e, d, c)
              else:
                if b<d:
                  if c<e:
                    if c<f:
                      return (b, d, a, c, f, e)
                    else:
                      return (b, d, a, f, c, e)
                  else:
                    return (b, d, a, f, e, c)
                else:
                  if c<e:
                    if c<f:
                      return (d, b, a, c, f, e)
                    else:
                      return (d, b, a, f, c, e)
                  else:
                    return (d, b, a, f, e, c)
            else:
              if d<f:
                if b<d:
                  if a<e:
                    if c<e:
                      return (b, d, f, a, c, e)
                    else:
                      return (b, d, f, a, e, c)
                  else:
                    return (b, d, f, e, a, c)
                else:
                  if a<e:
                    if c<e:
                      return (d, b, f, a, c, e)
                    else:
                      return (d, b, f, a, e, c)
                  else:
                    return (d, b, f, e, a, c)
              else:
                if a<d:
                  if d<e:
                    if c<e:
                      return (b, f, a, d, c, e)
                    else:
                      return (b, f, a, d, e, c)
                  else:
                    if a<e:
                      return (b, f, a, e, d, c)
                    else:
                      return (b, f, e, a, d, c)
                else:
                  if a<e:
                    if c<e:
                      return (b, f, d, a, c, e)
                    else:
                      return (b, f, d, a, e, c)
                  else:
                    if d<e:
                      return (b, f, d, e, a, c)
                    else:
                      return (b, f, e, d, a, c)
          else:
            if b<d:
              if a<d:
                if a<e:
                  if c<e:
                    return (f, b, a, d, c, e)
                  else:
                    if d<e:
                      return (f, b, a, d, e, c)
                    else:
                      return (f, b, a, e, d, c)
                else:
                  if b<e:
                    return (f, b, e, a, d, c)
                  else:
                    return (f, e, b, a, d, c)
              else:
                if a<e:
                  if c<e:
                    return (f, b, d, a, c, e)
                  else:
                    return (f, b, d, a, e, c)
                else:
                  if b<e:
                    if d<e:
                      return (f, b, d, e, a, c)
                    else:
                      return (f, b, e, d, a, c)
                  else:
                    return (f, e, b, d, a, c)
            else:
              if a<e:
                if c<e:
                  if d<f:
                    return (d, f, b, a, c, e)
                  else:
                    return (f, d, b, a, c, e)
                else:
                  if d<f:
                    return (d, f, b, a, e, c)
                  else:
                    return (f, d, b, a, e, c)
              else:
                if b<e:
                  if d<f:
                    return (d, f, b, e, a, c)
                  else:
                    return (f, d, b, e, a, c)
                else:
                  if d<e:
                    if d<f:
                      return (d, f, e, b, a, c)
                    else:
                      return (f, d, e, b, a, c)
                  else:
                    return (f, e, d, b, a, c)
      else:
        if e<f:
          if b<e:
            if b<d:
              if d<e:
                if c<e:
                  if a<e:
                    return (b, d, c, a, e, f)
                  else:
                    if a<f:
                      return (b, d, c, e, a, f)
                    else:
                      return (b, d, c, e, f, a)
                else:
                  if a<f:
                    return (b, d, e, c, a, f)
                  else:
                    if c<f:
                      return (b, d, e, c, f, a)
                    else:
                      return (b, d, e, f, c, a)
              else:
                if c<f:
                  if a<f:
                    return (b, e, d, c, a, f)
                  else:
                    return (b, e, d, c, f, a)
                else:
                  if d<f:
                    return (b, e, d, f, c, a)
                  else:
                    return (b, e, f, d, c, a)
            else:
              if a<f:
                if b<c:
                  if a<e:
                    return (d, b, c, a, e, f)
                  else:
                    if c<e:
                      return (d, b, c, e, a, f)
                    else:
                      return (d, b, e, c, a, f)
                else:
                  if a<e:
                    return (d, c, b, a, e, f)
                  else:
                    return (d, c, b, e, a, f)
              else:
                if c<e:
                  if b<c:
                    return (d, b, c, e, f, a)
                  else:
                    return (d, c, b, e, f, a)
                else:
                  if c<f:
                    return (d, b, e, c, f, a)
                  else:
                    return (d, b, e, f, c, a)
          else:
            if b<c:
              if c<f:
                if a<f:
                  if b<d:
                    return (e, b, d, c, a, f)
                  else:
                    if d<e:
                      return (d, e, b, c, a, f)
                    else:
                      return (e, d, b, c, a, f)
                else:
                  if b<d:
                    return (e, b, d, c, f, a)
                  else:
                    if d<e:
                      return (d, e, b, c, f, a)
                    else:
                      return (e, d, b, c, f, a)
              else:
                if b<f:
                  if b<d:
                    if d<f:
                      return (e, b, d, f, c, a)
                    else:
                      return (e, b, f, d, c, a)
                  else:
                    if d<e:
                      return (d, e, b, f, c, a)
                    else:
                      return (e, d, b, f, c, a)
                else:
                  if d<f:
                    if d<e:
                      return (d, e, f, b, c, a)
                    else:
                      return (e, d, f, b, c, a)
                  else:
                    if b<d:
                      return (e, f, b, d, c, a)
                    else:
                      return (e, f, d, b, c, a)
            else:
              if b<f:
                if a<f:
                  if c<e:
                    return (d, c, e, b, a, f)
                  else:
                    if d<e:
                      return (d, e, c, b, a, f)
                    else:
                      return (e, d, c, b, a, f)
                else:
                  if c<e:
                    return (d, c, e, b, f, a)
                  else:
                    if d<e:
                      return (d, e, c, b, f, a)
                    else:
                      return (e, d, c, b, f, a)
              else:
                if d<e:
                  if c<e:
                    return (d, c, e, f, b, a)
                  else:
                    if c<f:
                      return (d, e, c, f, b, a)
                    else:
                      return (d, e, f, c, b, a)
                else:
                  if c<f:
                    return (e, d, c, f, b, a)
                  else:
                    if d<f:
                      return (e, d, f, c, b, a)
                    else:
                      return (e, f, d, c, b, a)
        else:
          if b<f:
            if b<d:
              if a<e:
                if c<f:
                  if a<f:
                    return (b, d, c, a, f, e)
                  else:
                    return (b, d, c, f, a, e)
                else:
                  if d<f:
                    return (b, d, f, c, a, e)
                  else:
                    return (b, f, d, c, a, e)
              else:
                if c<e:
                  if c<f:
                    return (b, d, c, f, e, a)
                  else:
                    if d<f:
                      return (b, d, f, c, e, a)
                    else:
                      return (b, f, d, c, e, a)
                else:
                  if d<e:
                    if d<f:
                      return (b, d, f, e, c, a)
                    else:
                      return (b, f, d, e, c, a)
                  else:
                    return (b, f, e, d, c, a)
            else:
              if a<e:
                if b<c:
                  if a<f:
                    return (d, b, c, a, f, e)
                  else:
                    if c<f:
                      return (d, b, c, f, a, e)
                    else:
                      return (d, b, f, c, a, e)
                else:
                  if a<f:
                    return (d, c, b, a, f, e)
                  else:
                    return (d, c, b, f, a, e)
              else:
                if c<f:
                  if b<c:
                    return (d, b, c, f, e, a)
                  else:
                    return (d, c, b, f, e, a)
                else:
                  if c<e:
                    return (d, b, f, c, e, a)
                  else:
                    return (d, b, f, e, c, a)
          else:
            if b<c:
              if c<e:
                if a<e:
                  if b<d:
                    return (f, b, d, c, a, e)
                  else:
                    if d<f:
                      return (d, f, b, c, a, e)
                    else:
                      return (f, d, b, c, a, e)
                else:
                  if b<d:
                    return (f, b, d, c, e, a)
                  else:
                    if d<f:
                      return (d, f, b, c, e, a)
                    else:
                      return (f, d, b, c, e, a)
              else:
                if b<e:
                  if b<d:
                    if d<e:
                      return (f, b, d, e, c, a)
                    else:
                      return (f, b, e, d, c, a)
                  else:
                    if d<f:
                      return (d, f, b, e, c, a)
                    else:
                      return (f, d, b, e, c, a)
                else:
                  if d<e:
                    if d<f:
                      return (d, f, e, b, c, a)
                    else:
                      return (f, d, e, b, c, a)
                  else:
                    if b<d:
                      return (f, e, b, d, c, a)
                    else:
                      return (f, e, d, b, c, a)
            else:
              if b<e:
                if a<e:
                  if c<f:
                    return (d, c, f, b, a, e)
                  else:
                    if d<f:
                      return (d, f, c, b, a, e)
                    else:
                      return (f, d, c, b, a, e)
                else:
                  if c<f:
                    return (d, c, f, b, e, a)
                  else:
                    if d<f:
                      return (d, f, c, b, e, a)
                    else:
                      return (f, d, c, b, e, a)
              else:
                if c<e:
                  if c<f:
                    return (d, c, f, e, b, a)
                  else:
                    if d<f:
                      return (d, f, c, e, b, a)
                    else:
                      return (f, d, c, e, b, a)
                else:
                  if d<e:
                    if d<f:
                      return (d, f, e, c, b, a)
                    else:
                      return (f, d, e, c, b, a)
                  else:
                    return (f, e, d, c, b, a)

print f(3, 1, 5, 0, 2, 4)
print f(4, 0, 2, 3, 1, 5)
print f(3, 1, 4, 5, 0, 2)
print f(1, 5, 0, 3, 4, 2)
print f(5, 4, 1, 0, 2, 3)
print f(5, 1, 2, 0, 3, 4)
print f(3, 5, 4, 0, 2, 1)
print f(2, 3, 1, 0, 4, 5)
print f(4, 2, 5, 1, 0, 3)
print f(1, 3, 2, 0, 4, 5)
