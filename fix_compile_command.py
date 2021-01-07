import os
import sys
import subprocess

data = eval(''.join(open(sys.argv[1]).readlines()))
extra_inc_path = sys.argv[2] if len(sys.argv) >= 3 else None

print('[')

first = True

for entry in data:
    if not first:
        print(',')
    f = '%s/%s' % (entry['directory'], entry['file'])
    len_prefix = len(entry['directory'])
    if os.path.islink(f):
        raw = subprocess.check_output('ls -l %s' % f, shell=True).decode('utf-8')
        assert '->' in raw
        tokens = raw.split()
        idx = tokens.index('->')
        entry['file'] = tokens[idx + 1][len_prefix+1:]
        entry['arguments'][-1] = tokens[idx + 1][len_prefix+1:]
        print('{\n\t"directory": "%s",\n\t"file": "%s",' % (entry['directory'], tokens[idx + 1][len_prefix+1:]))
    else:
        print('{\n\t"directory": "%s",\n\t"file": "%s",' % (entry['directory'], entry['file']))
    print('\t"arguments": [')
    res = []
    for elem in entry['arguments']:
        if '"' in elem:
            elem = elem.replace('"', chr(92) + chr(34))
        s = '\t\t"%s"' % elem
        res.append(s)
    if extra_inc_path:
        # Add extra include path here.
        res.append(f'\t\t"-I{extra_inc_path}"')
    # We always add the directory to the include path.
    res.append(f'\t\t"-I{os.path.dirname(f)}"')
    print(',\n'.join(res))
    print('\t]')
    print('}', end='')
    first = False

print(']')