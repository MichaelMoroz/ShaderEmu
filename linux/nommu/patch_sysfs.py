"""No MMU: sysfs folders nothing reads, left out of the boot (GPU-VM's d7996f4, there behind CONFIG_GPUVM): a third
of gpu_vm's kernel start. /sys/kernel/slab for every cache made before slab_sysfs_init (a cache made later still
gets its folder) and /sys/module for the built-in parameters and versions.   python3 patch_sysfs.py K"""
import sys
K = sys.argv[1]


def patch(rel, old, new):
    p = K + '/' + rel
    s = open(p).read()
    if new in s:
        return
    assert s.count(old) == 1, (rel, old)
    open(p, 'w').write(s.replace(old, new))


patch('mm/slub.c', '\tslab_state = FULL;\n\n\tlist_for_each_entry(s, &slab_caches, list) {\n',
      '\tslab_state = FULL;\n\n\t/* (shaderemu: no folder for the boot\'s caches) */\n\twhile (alias_list) {\n'
      '\t\tstruct saved_alias *al = alias_list;\n\n\t\talias_list = al->next;\n\t\tkfree(al);\n\t}\n'
      '\tmutex_unlock(&slab_mutex);\n\treturn 0;\n\n\tlist_for_each_entry(s, &slab_caches, list) {\n')
patch('kernel/params.c', '\tversion_sysfs_builtin();\n\tparam_sysfs_builtin();\n',
      '\t/* (shaderemu: no /sys/module for what is built in) */\n')
print('no sysfs for the boot\'s slab caches and built-in parameters')
