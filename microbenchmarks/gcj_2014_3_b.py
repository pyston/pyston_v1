import sys

if __name__ == "__main__":
    f = sys.stdin
    if len(sys.argv) >= 2:
        fn = sys.argv[1]
        if fn != '-':
            f = open(fn)

    # test data is from the Google CodeJam site:
    s = """1\n22 20 100\n195 649898\n191 583577\n190 70961\n192 608854\n193 43725\n193 647827\n193 339043\n190 270370\n191 68136\n198 505964\n193 20802\n191 285258\n191 80670\n193 183965\n194 16065\n190 76452\n197 91485\n199 170286\n190 445988\n193 324817\n190 87804\n193 6882\n193 226516\n190 465689\n194 146167\n192 127049\n191 176172\n192 396031\n192 398308\n192 108078\n190 13378\n192 38714\n191 589111\n192 437328\n193 618750\n190 14397\n190 704848\n194 48882\n192 574139\n191 480711\n194 357559\n191 63176\n193 175447\n194 258453\n191 706947\n190 12511\n191 181170\n192 21592\n195 359968\n193 92393\n199 116308\n190 352072\n195 402952\n191 135017\n192 277287\n190 129665\n192 428804\n194 51461\n190 171899\n192 69424\n192 638021\n190 224689\n190 118853\n191 82341\n192 172974\n199 8643\n192 34032\n190 166043\n190 228990\n192 79741\n200 183198\n191 348403\n195 22955\n198 258715\n192 389063\n191 283406\n192 519332\n190 40562\n191 62096\n191 475046\n192 22699\n190 39911\n197 268042\n192 170463\n192 108990\n191 357464\n191 144522\n192 461237\n194 327871\n190 67947\n190 748144\n190 106941\n194 44355\n193 81455\n191 65589\n190 486823\n200 375651\n192 636531\n190 18524\n192 38697"""
    l = s.split('\n')

    T = int(l.pop(0))
    for _T in xrange(T):
        P, Q, N = map(int, l.pop(0).split())

        healths = []
        golds = []

        for i in xrange(N):
            h, g = map(int, l.pop(0).split())
            healths.append(h)
            golds.append(g)

        def num_hits_to_win(h):
            assert h > 0

            if P > Q:
                return 1

            t = 1
            while True:
                m = ((h - 1) % Q) + 1
                if 1 <= m <= P:
                    return t

                h -= P
                t += 1

        # for h in healths:
            # print h, num_hits_to_win(h)

        cur = {1: 0}

        for i in xrange(N):
            next = {}

            hits_needed = num_hits_to_win(healths[i])
            would_win = golds[i]

            # print
            # print "On creep %d" % i

            for shots, money in cur.iteritems():
                # print "Could have %d shots and %d gold" % (shots, money)
                do_nothing = (healths[i] + Q-1) / Q + shots

                # print "do nothing and would have %d shots" % do_nothing
                next[do_nothing] = max(next.get(do_nothing, 0), money)

                health = healths[i]
                if shots == 0:
                    health -= Q
                    shots += 1
                    if health <= 0:
                        continue

                # min_tower_shots = hits_needed - shots
                # if min_tower_shots * Q + (hits_needed - 1) * P > health:
                    # continue

                tower_shots = max((health - (hits_needed - 1) * P - 1) / Q, 0)
                next_shots = shots - hits_needed + tower_shots
                # print "tower shoots %d times, we shoot %d times; we'd have %d shots" % (tower_shots, hits_needed, next_shots)
                if next_shots < 0:
                    # print "we only have %d initial shots"
                    continue

                next[next_shots] = max(next.get(next_shots, 0), money + would_win)

            cur = next

        # print cur
        best = max(cur.itervalues())
        print "Case #%d: %d" % (_T+1, best)


