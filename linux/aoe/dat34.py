"""The trial's Empires.dat as the code reads it. For now the file itself."""
import shutil

def convert(src, dst):
    shutil.copyfile(src, dst)
