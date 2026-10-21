"""Has the kernel's tick poll the machine's input (shaderemu_input.c) and network
(shaderemu_net.c), and the idle loop arm those drivers' timers: a timer of its own that expired
at every tick cost a running program 900 instructions a tick in the timer wheel, for a poll of
30. Idempotent."""
import sys
tree = sys.argv[1]
CALL = '#ifdef CONFIG_%s\n\t{\n\t\textern void %s(void);\n\t\t%s();\n\t}\n#endif\n'
TICK = ('/kernel/time/timer.c', '\taccount_process_tick(p, user_tick);\n')
IDLE = ('/kernel/sched/idle.c', '\ttick_nohz_idle_enter();\n')
for (path, before), config, name in (
        (TICK, 'INPUT_SHADEREMU', 'shaderemu_input_tick'), (IDLE, 'INPUT_SHADEREMU', 'shaderemu_input_idle'),
        (TICK, 'SHADEREMU_NET', 'shaderemu_net_tick'), (IDLE, 'SHADEREMU_NET', 'shaderemu_net_idle')):
    s = open(tree + path).read()
    if name in s:
        continue
    assert s.count(before) == 1, path
    open(tree + path, 'w').write(s.replace(before, CALL % (config, name, name) + before))
    print(name, 'hook added to', path)
