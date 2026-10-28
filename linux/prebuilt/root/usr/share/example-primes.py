# The primes below a limit, by the sieve of Eratosthenes. F9 marks a line, F5 runs to it,
# F10 steps over a line and F11 into sieve().
LIMIT = 100


def sieve(limit):
    marked = [False] * limit
    found = []
    for n in range(2, limit):
        if not marked[n]:
            found.append(n)
            for multiple in range(n * n, limit, n):
                marked[multiple] = True
    return found


primes = sieve(LIMIT)
print(len(primes), "primes below", LIMIT)
print(primes)
