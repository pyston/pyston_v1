import sys

if __name__ == "__main__":
    # test data is from the Google CodeJam site:
    s = """
5
66 150 4
0 67 54 133
55 20 60 67
61 63 64 67
65 63 65 85
64 21 8
32 1 55 13
42 14 61 19
16 6 26 20
11 1 13 3
14 2 30 4
28 9 30 16
5 14 14 17
11 4 13 6
99 321 10
0 52 16 52
22 52 38 52
83 52 98 52
16 105 82 105
0 158 16 158
31 158 47 158
83 158 98 158
15 211 81 211
0 264 16 264
20 264 36 264
6 313 1
0 157 5 286
76 119 8
13 17 72 80
25 98 38 117
50 116 63 118
49 2 66 12
61 92 66 105
1 52 7 105
9 80 9 91
45 113 59 114
22 437 1
0 187 21 315
    """.strip()

    l = s.split('\n')

    T = int(l.pop(0))
    for _T in xrange(T):
        W, H, B = map(int, l.pop(0).split())

        buildings = []
        for i in xrange(B):
            buildings.append(map(int, l.pop(0).split()))

        grid = [[1] * H for i in xrange(W)]

        for x0, y0, x1, y1 in buildings:
            for x in xrange(x0, x1+1):
                for y in xrange(y0, y1+1):
                    grid[x][y] = 0

        r = 0
        for sx in xrange(W):
            if grid[sx][0] == 0:
                continue

            cx, cy = sx, 0
            dx, dy = 0, 1

            visited = []
            while True:
                # print "at", cx, cy

                visited.append((cx, cy))

                if cy == H-1:
                    for x, y in visited:
                        grid[x][y] = 0
                    r += 1
                    # print "made it!"
                    break

                dx, dy = -dy, dx
                failed = False
                while True:
                    nx, ny = cx + dx, cy + dy
                    if nx >= 0 and nx < W and ny >= 0 and ny < H and grid[nx][ny]:
                        cx, cy = nx, ny
                        break
                    else:
                        if (cx, cy) == (sx, 0) and (dx, dy) == (0, -1):
                            failed = True
                            break

                        dx, dy = dy, -dx

                if failed:
                    for x, y in visited:
                        grid[x][y] = 0
                    # print "failed"
                    break

                # print "moved", dx, dy

        print "Case #%d: %d" % (_T+1, r)
