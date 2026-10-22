"""Runs a command and prints each line of its output with this computer's seconds since the start (the console's
lines of a run on the card, which flushes them every batch).
    python stamp.py [--until TEXT] [--log FILE] -- COMMAND [ARGS...]
--until: ends the command at the first line with TEXT."""
import subprocess, sys, time

args = sys.argv[1:]
until, log = None, None
while args and args[0] != '--':
    if args[0] == '--until': until = args[1]; args = args[2:]
    elif args[0] == '--log': log = open(args[1], 'w', encoding='utf-8'); args = args[2:]
    else: sys.exit('stamp.py: what is ' + args[0])
args = args[1:]
t0 = time.perf_counter()
p = subprocess.Popen(args, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, stdin=subprocess.DEVNULL)
code = None
for raw in iter(p.stdout.readline, b''):
    line = '%9.3f %s' % (time.perf_counter() - t0, raw.decode('utf-8', 'replace').rstrip('\r\n'))
    print(line, flush=True)
    if log: log.write(line + '\n'); log.flush()
    if until and until in line:
        p.terminate()
        code = 0
        break
r = p.wait()
sys.exit(code if code is not None else r)
