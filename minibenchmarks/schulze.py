# Read the votes
import os
fp = open(os.path.join(os.path.dirname(__file__), 'schulze.votes'), 'r')
lines = fp.readlines()
fp.close()

# Strip superfluous information
lines = [line[14:].rstrip() for line in lines]

votes = [line.split(' ') for line in lines]

# Make a canonical set of all the candidates
candidates = {}
for line in votes:
    for memberId in line:
        candidates[memberId] = 1

# Go from member number to an index
for i, k in enumerate(candidates.keys()):
    candidates[k] = i

# And vice versa
reverseCandidates = {}
for k, v in candidates.items():
    reverseCandidates[v] = k

# Turn the votes in to an index number
numbers = [[candidates[memberId] for memberId in line] for line in votes]

size = len(candidates)

# Initialize the d and p matrixes
row = []
for i in range(size):
    row.append(0)

d = []
p = []
for i in range(size):
    d.append(row[:])
    p.append(row[:])

# Fill in the preferences in the d matrix
for i in range(size):
    for line in numbers:
        for entry in line:
            if entry == i:
                break
            d[entry][i] += 1

# Calculate the p matrix. Algorithm copied straight from wikipedia
# article http://en.wikipedia.org/wiki/Schulze_method
for i in range(size):
    for j in range(size):
        if i != j:
            if d[i][j] > d[j][i]:
                p[i][j] = d[i][j]
            else:
                p[i][j] = 0

for i in range(size):
    for j in range(size):
        if i != j:
            for k in range(size):
                if i != k:
                    if j != k:
                        p[j][k] = max(p[j][k], min(p[j][i], p[i][k]))

# Find the best candidate (p[candidate, X] >= p[X, candidate])
# Put the candidate on the final list, remove the candidate from p and
# repeat
order = []
cl = range(size)

while cl:
    for c in cl:
        for n in cl:
            if c != n:
                if p[c][n] < p[n][c]:
                    break # Found a better candidate
        else:
            order.append(c)
            cl.remove(c)

# Display the results
j = 0
for i in order:
    j += 1
    print '%3d %s' % (j, reverseCandidates[i])
