# The IDE's debugger for Python (docs/apps/ide.md): runs FILE under sys.settrace, stops at its
# marked lines and steps, told what to do through COMMANDS and saying where it stopped to REPORT.
#   micropython /usr/share/ide-pydbg.py FILE COMMANDS REPORT
import sys

file, commands, report = sys.argv[1], sys.argv[2], sys.argv[3]
inbox = open(commands)
outbox = open(report, 'w')
marks = set()
mode = 'into'	# go, into, over, out
depth_from = 0	# the frames of FILE where a step began
SKIP = ('module', 'function', 'type', 'bound_method', 'closure', 'generator')


def depth(frame):
    n = 0
    while frame:
        if frame.f_code.co_filename == file:
            n += 1
        frame = getattr(frame, "f_back", None)	# (none for a frame made before the trace)
    return n


def say(line):
    outbox.write(line + '\n')	# (written at once: flush() is fsync(), which a FIFO refuses)


def shown(value):
    try:
        text = repr(value)
    except Exception:
        text = '?'
    return text if len(text) < 120 else text[:117] + '...'


def stop(frame):
    """Says where it is, then waits for what to do next."""
    global mode, depth_from, marks
    say('stop %d' % frame.f_lineno)
    g = frame.f_globals
    for name in sorted(g):
        value = g[name]
        if not name.startswith('__') and type(value).__name__ not in SKIP:
            say('var %s = %s' % (name, shown(value)))
    f = frame
    while f:
        if f.f_code.co_filename == file:
            name = f.f_code.co_name
            say('frame %s, line %d' % ('the module' if name == '<module>' else name, f.f_lineno))
        f = getattr(f, "f_back", None)
    say('end')
    while True:
        words = inbox.readline().split()
        if not words:
            raise SystemExit(0)	# the IDE went
        if words[0] == 'marks':
            marks = set(int(w) for w in words[1:])
        elif words[0] in ('go', 'into', 'over', 'out'):
            mode, depth_from = words[0], depth(frame)
            return
        elif words[0] == 'stop':
            raise SystemExit(0)


def trace(frame, event, arg):
    if frame.f_code.co_filename != file:
        return None	# (another module's lines are not traced at all)
    if event == 'line':
        line = frame.f_lineno
        here = depth(frame) if mode in ('over', 'out') else 0
        if line in marks or mode == 'into' or (mode == 'over' and here <= depth_from) or (mode == 'out' and here < depth_from):
            stop(frame)
    return trace


def main():
    global marks, mode
    words = inbox.readline().split()
    if words and words[0] == 'marks':
        marks = set(int(w) for w in words[1:])
    mode = 'go' if marks else 'into'
    source = open(file).read()
    code = compile(source, file, 'exec')
    scope = {'__name__': '__main__', '__file__': file}
    sys.settrace(trace)
    try:
        exec(code, scope)
    except SystemExit as e:
        sys.settrace(None)
        code = e.args[0] if e.args and isinstance(e.args[0], int) else 0
        say('exit %d' % code)
        return code
    except BaseException as e:
        sys.settrace(None)
        sys.print_exception(e)
        say('exit 1')
        return 1
    sys.settrace(None)
    say('exit 0')
    return 0


sys.exit(main())
