#!/usr/bin/env python3
"""The game's tree as GCC and musl read it, from upstream's plus aoe.patch: tree.py SRC OUT.

The sources declare the C library's functions themselves, in Visual C++'s forms (`extern "C" int
__cdecl open(const char*, int);`, in a dozen files): those lines go, musl's headers say the same.
Names of headers are in another case here and there, which a Linux file system minds.
Only what changed is written, so that a build compiles only that."""
import os, re, sys

src, out = sys.argv[1], sys.argv[2]
CRT = r'(?:open|_open|close|_close|read|_read|write|_write|lseek|_lseek|tell|_tell|unlink|_unlink|fclose|fopen|fread|fwrite|fgets|fscanf|fprintf|sprintf|sscanf|printf|memcpy|memset|memcmp|memmove|strlen|strcpy|strcmp|strcat|strchr|strstr|strncpy|malloc|calloc|free|realloc|time|ctime|localtime|gmtime|mktime|rand|srand|atoi|atof|exit|access|_access|_findfirst|_findnext|_findclose|_ftol|sqrt|sin|cos|atan2|floor|ceil|fabs|fmod|pow|abs|labs|qsort|toupper|tolower|isspace|isdigit|isalpha|_rotl|_rotr|getcwd|chdir)'
DECL = re.compile(r'^\s*(?:extern\s+(?:"C"\s+)?)?[\w\s\*]*?\b(?:__cdecl\s+)' + CRT + r'\s*\([^;{]*\)\s*;\s*$')
HEADERS = {n.lower(): n for n in os.listdir(os.path.join(src, 'include'))}
INC = re.compile(r'(#include\s+"(?:\.\./include/)?)([^"/]+)"')

NOSIZE = re.compile(r'^typedef char (?:rge_action_object_size_check|rge_combat_object_vur_offset_check|rge_combat_object_size_check|tribe_combat_object_size_check|tribe_combat_object_own_master_offset_check|tribe_building_object_size_check|rge_missile_object_size_check|rge_missile_object_max_range_offset_check|tribe_action_make_tech_size_check|tact_mob_make_obj_size|tact_rep_action_size_check|tact_cnv_size_check|com_addr_size_check)\b.*$', re.M)

def inc(m):
    return m.group(1) + HEADERS.get(m.group(2).lower(), m.group(2)) + '"'

def fix(text):
    text = INC.sub(inc, text).replace('__asm int 3', 'abort();').replace('extern "C" static', 'static')
    text = text.replace('extern "C" TDebuggingLog* L;', 'extern TDebuggingLog* L;').replace('extern "C" int restore_mouse_after_paint', 'extern int restore_mouse_after_paint')
    # GCC lays a derived class's fields into its base's tail padding, Visual C++ does not: sizes of
    # the objects are smaller here, and nothing goes by them (each is saved field by field)
    text = NOSIZE.sub(lambda m: '// ' + m.group(0), text)
    return '\n'.join('// ' + l if DECL.match(l) else l for l in text.split('\n'))

for sub in ('src', 'include'):
    os.makedirs(os.path.join(out, sub), exist_ok=True)
    for name in os.listdir(os.path.join(src, sub)):
        if not name.endswith(('.cpp', '.c', '.h')):
            continue
        text = open(os.path.join(src, sub, name), 'rb').read().decode('latin1').replace('\r\n', '\n')
        data = fix(text).encode('latin1')
        path = os.path.join(out, sub, name)
        if not os.path.exists(path) or open(path, 'rb').read() != data:
            open(path, 'wb').write(data)
