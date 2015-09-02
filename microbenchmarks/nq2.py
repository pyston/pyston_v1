# From: https://mail.python.org/pipermail/pypy-dev/2014-August/012695.html

L = 10

xrows = range(L)
xcols = range(L)

bitmap = [0] * L ** 2

poss = [(i, j) for i in xrows for j in xcols]

idx_to_pos = dict()
pos_to_idx = dict()

for i, pos in enumerate(poss):
    idx_to_pos[i] = pos
    pos_to_idx[pos] = i

# rows, columns, "right" diagonals and "left" diagonals
poscols = [[(i, j) for i in xrows] for j in xcols]
posrows = [[(i, j) for j in xcols] for i in xrows]
posdiag = [[(h, g - h) for h in range(g + 1) if h < L and g - h < L] for g in range(L * 2 - 1)]
posgaid = [[(g + h, h) for h in range(L) if -1 < g + h < L] for g in range(-L + 1, L)]


def attacks(pos):
    """ all attacked positions """
    row = filter(lambda r: pos in r, posrows)
    col = filter(lambda c: pos in c, poscols)
    dia = filter(lambda d: pos in d, posdiag)
    gai = filter(lambda g: pos in g, posgaid)
    assert len(row) == len(col) == len(dia) == len(gai) == 1
    return frozenset(row[0]), frozenset(col[0]), frozenset(dia[0]), frozenset(gai[0])

attackmap = {(i, j): attacks((i, j)) for i in range(L) for j in range(L)}

setcols = set(map(frozenset, poscols))
setrows = set(map(frozenset, posrows))
setdiag = set(map(frozenset, posdiag))
setgaid = set(map(frozenset, posgaid))

# choice between bitmaps and sets
#
# bitmaps are reresented natively as (long) ints in Python,
# thus bitmap operations are very very fast
#
# however for asymptotic complexity, x in bitmap operation is O(N) and x in set is O(logN)
#
# in my experience python function calls are expensive, thus the threshold where sets show benefit is rather high
# another possible explanation for high threshold is large memory size of Python dictionaries and thus frozensets,
# __sizeof__ for representaions of range(100):: set: 8K, frozenset: 4K, (2 ** 100): 40 bytes
#
# for 8x8 board, a 64-bit bitmap wins by a large margin
# IMO 10x10 board is still faster with bitmaps


# all queens are equivalent, thus solution (Q1, Q2, Q3) == (Q1, Q3, Q2)
# let's order queens, so that Q1 always preceeds on Q2 on the board
# then, let's do an exhaustive search with early pruning:
# consider board of 4 [ , , , ] for 3 queens
# position [ , ,Q1, ] will never generate a solution, because there's no space for both Q2 and Q3 left
# likewise, let's extend concept of "space" along 4 dimensions -- rows, cols, diag, gaid

solutions = []


def place(board, queens, r, c, d, g):
    """
    remaining unattacked places on the board
    remaining queens to place
    remaining rows, cols, diag, gaid free
    """
    # if we are ran out of queens, it's a valid solution
    if not queens:
        # print "solution found"
        solutions.append(None)

    # early pruning, make sure this many queens can actually be placed
    if len(queens) > len(board): return
    if len(queens) > len(r): return
    if len(queens) > len(c): return
    if len(queens) > len(d): return
    if len(queens) > len(g): return

    # queens[0] is queen to be places on some pos
    for ip, pos in enumerate(board):
        ar, ac, ad, ag = attackmap[pos]
        attacked = frozenset.union(ar, ac, ad, ag)
        nboard = [b for b in board[ip + 1:] if b not in attacked]
        place(nboard, queens[1:], r - ar, c - ac, d - ad, g - ag)


def run():
    del solutions[:]
    place(poss, sorted(["Q%s" % i for i in range(L)]), setrows, setcols, setdiag, setgaid)
    return len(solutions)

print(run())
