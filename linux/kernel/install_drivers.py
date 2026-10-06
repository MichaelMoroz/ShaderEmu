"""Copies this project's drivers into the kernel tree given as argv[1] and adds their Kconfig
and Makefile entries. Idempotent."""
import os, shutil, sys

here, tree = os.path.dirname(os.path.abspath(__file__)), sys.argv[1]
DRIVERS = [  # source, directory, config symbol, prompt, extra Kconfig lines
    ('shaderemu_gpu.c', 'drivers/misc', 'SHADEREMU_GPU', 'ShaderEmu GPU device', []),
    ('shaderemu_fb.c', 'drivers/video/fbdev', 'FB_SHADEREMU', 'ShaderEmu display',
     ['depends on FB', 'select FB_CFB_FILLRECT', 'select FB_CFB_COPYAREA', 'select FB_CFB_IMAGEBLIT']),
    ('shaderemu_input.c', 'drivers/input/misc', 'INPUT_SHADEREMU', 'ShaderEmu keyboard and pointer', []),
]
for source, directory, symbol, prompt, extra in DRIVERS:
    shutil.copy(os.path.join(here, source), os.path.join(tree, directory))
    makefile = os.path.join(tree, directory, 'Makefile')
    if symbol not in open(makefile).read():
        open(makefile, 'a').write('obj-$(CONFIG_%s) += %s\n' % (symbol, source.replace('.c', '.o')))
    kconfig = os.path.join(tree, directory, 'Kconfig')
    text = open(kconfig).read()
    if symbol not in text:
        entry = 'config %s\n\tbool "%s"\n%s\n' % (symbol, prompt, ''.join('\t%s\n' % line for line in extra))
        # inside the file's own menu or "if" block when it ends with one, else at the end
        body = text.rstrip()
        for closing in ('endmenu', 'endif'):
            if body.endswith(closing):
                at = body.rfind('\n' + closing)
                text = body[:at + 1] + entry + body[at + 1:] + '\n'
                break
        else:
            text = body + '\n\n' + entry
        open(kconfig, 'w').write(text)
