"""No-MMU kernel: a dying program's worker cores parked, and the pass ended, before its memory goes
(shaderemu_gpu_exiting in drivers/misc/shaderemu_gpu.c, called in do_exit ahead of exit_mm). Its
release, after exit_mm, parked them late: the workers ran on in freed memory for the passes between."""
import sys
K = sys.argv[1]
p = f'{K}/kernel/exit.c'
s = open(p).read()
if 'shaderemu_gpu_exiting' not in s:
    old = '\texit_mm();\n'
    assert s.count(old) == 1
    s = s.replace(old, '#ifdef CONFIG_SHADEREMU_GPU\n\t{\n\t\textern void shaderemu_gpu_exiting(void);\n\n'
                       '\t\tshaderemu_gpu_exiting();\t/* its worker cores out of its memory first */\n\t}\n#endif\n' + old)
    open(p, 'w').write(s)
