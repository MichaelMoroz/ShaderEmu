"""No SMP: the tick advances the timekeeper where it lies, not a shadow copied into it whole (docs/plans/riscv-profile.md,
item 6: timekeeping_advance and timekeeping_update 2.5% and 2.3% of core 0's write-cache entries, the top kernel
functions of an idle desktop).

timekeeping_advance works on shadow_timekeeper under timekeeper_lock with interrupts off, then copies all of it (about
300 bytes) into tk_core.timekeeper under tk_core.seq, so that on SMP a reader on another processor retries only for
the copy. With one processor and interrupts off nothing reads the timekeeper while the tick changes it: the tick
changes tk_core.timekeeper itself, inside the same seqcount, and the shadow is neither copied to nor from (the other
writers' mirror, TK_MIRROR, kept it equal to the real one only for the tick).   python3 patch_timekeeping.py K"""
import sys
K = sys.argv[1]
MARK = 'shaderemu timekeeping'
p = K + '/kernel/time/timekeeping.c'
s = open(p).read()
if MARK not in s:
    for old, new in (
        ('''	struct timekeeper *real_tk = &tk_core.timekeeper;
	struct timekeeper *tk = &shadow_timekeeper;
''', '''	struct timekeeper *real_tk = &tk_core.timekeeper;
	/* shaderemu timekeeping: with one processor and interrupts off nothing reads it meanwhile */
	struct timekeeper *tk = IS_ENABLED(CONFIG_SMP) ? &shadow_timekeeper : real_tk;
'''),
        ('''	timekeeping_update(tk, clock_set);
	memcpy(real_tk, tk, sizeof(*tk));
''', '''	timekeeping_update(tk, clock_set);
	if (IS_ENABLED(CONFIG_SMP))
		memcpy(real_tk, tk, sizeof(*tk));
'''),
        ('''	if (action & TK_MIRROR)
		memcpy(&shadow_timekeeper, &tk_core.timekeeper,
''', '''	if (IS_ENABLED(CONFIG_SMP) && (action & TK_MIRROR))
		memcpy(&shadow_timekeeper, &tk_core.timekeeper,
'''),
        # (every 16th: a reader adds the clock's cycles since anyway, and only the coarse clocks go by this: a file's date
        # is a third of a second old at most. It was a third of a tick's 2,000 instructions, 1% of every program)
        ('''void update_wall_time(void)
{
''', '''void update_wall_time(void)
{
	static unsigned int calls;

	if (++calls & 15)
		return;
'''),
    ):
        assert s.count(old) == 1, old
        s = s.replace(old, new)
    open(p, 'w', newline='\n').write(s)
print('timekeeping: the tick advances the timekeeper in place')
