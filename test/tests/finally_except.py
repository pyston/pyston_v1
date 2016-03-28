# these may seem pointless, but they exercise a family of corner cases in our
# CFG generation pass (cfg.cpp).
def returner():
    try:
        try:
            print '2 + 2'
            return
        finally:
            print 'finally'
    except:
        print 'exception'

returner()

def continuer():
    for x in [1]:
        try:
            try:
                print '2 + 2'
                continue
            finally:
                print 'finally'
        except:
            print 'exception'

continuer()

def breaker():
    for x in [1]:
        try:
            try:
                print '2 + 2'
                break
            finally:
                print 'finally'
        except:
            print 'exception'

breaker()

def raiser():
    try:
        try:
            print '2 + 2'
            raise Exception('blaaargh')
        finally:
            print 'finally'
    except:
        print 'exception'

raiser()

def alltogethernow():
    for x in [1,2,3,4]:
        try:
            try:
                print '2 + 2'
                if x == 1: break
                if x == 2: continue
                if x == 3: raise Exception('blaargh')
                if x == 4: return
            finally:
                print 'finally'
        except:
            print 'exception'

alltogethernow()
