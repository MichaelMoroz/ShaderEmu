"""build/images/gamebench_rv: linux/nommu/image.py's image of the prebuilt no-MMU root with /usr/bin/gamebench added
(build/gamebench/root/usr/bin/gamebench, made by build.sh).   HALFLIFE_DATA=0 python gpu_vm/tick/gamebench_rv/image.py"""
import os, runpy, shutil, sys
ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__)))))
GB = os.path.join(ROOT, 'build', 'gamebench', 'root', 'usr', 'bin', 'gamebench')
copytree = shutil.copytree
def with_bench(src, dst, *a, **k):
    r = copytree(src, dst, *a, **k)
    if dst.endswith('overlay'):
        shutil.copy(GB, os.path.join(dst, 'usr', 'bin', 'gamebench'))
    return r
shutil.copytree = with_bench
os.environ.setdefault('HALFLIFE_DATA', '0')
os.chdir(ROOT)
sys.argv = ['image.py', 'gamebench_rv']
runpy.run_path(os.path.join(ROOT, 'linux', 'nommu', 'image.py'), run_name='__main__')
