#!/usr/bin/env python3
"""Rebuild toollib/index.json (sha1 of every file, and a serial that counts the changes) and the toollib section of
data/tools.qrc. Run after adding or editing a tool:  python3 toollib/make_index.py
Layout: toollib/<kind>/<6 hex id>.json (one tool each), toollib/<kind>/kind.png
(the sketch for that kind), toollib/materials.json."""
import hashlib, json, os, re, sys
root = os.path.dirname(os.path.abspath(__file__))
repo = os.path.dirname(root)
files = {}
for dp, dn, fn in os.walk(root):
    for f in fn:
        rel = os.path.relpath(os.path.join(dp, f), root).replace(os.sep, '/')
        if rel == 'index.json' or not re.search(r'\.(json|png)$', rel):
            continue
        files[rel] = hashlib.sha1(open(os.path.join(dp, f), 'rb').read()).hexdigest()
ids = {}
for rel in files:
    m = re.match(r'^([a-z0-9_-]+)/([0-9a-f]{6})\.json$', rel)
    if m:
        d = json.load(open(os.path.join(root, rel)))
        if d.get('id') != m.group(2):
            sys.exit('%s: id field must equal the file name' % rel)
        if m.group(2) in ids:
            sys.exit('duplicate id ' + m.group(2))
        ids[m.group(2)] = rel
files = dict(sorted(files.items()))
# `serial` goes up by one whenever the files change. The app uses a synced
# copy of the library only while its serial is above the built-in one's, so a
# change that is not counted here never reaches an installed app.
ix = os.path.join(root, 'index.json')
try:
    was = json.load(open(ix))
except (OSError, ValueError):
    was = {}
serial = int(was.get('serial', 0))
if was.get('files') != files or serial < 1:
    serial += 1
open(ix, 'w').write(json.dumps({'version': 1, 'serial': serial, 'files': files}, indent=1) + '\n')
q = os.path.join(repo, 'data', 'tools.qrc')
s = open(q).read()
s = re.sub(r'\n    <file alias="toollib/[^\n]*', '', s)
add = ''.join('\n    <file alias="toollib/%s">../toollib/%s</file>' % (r, r) for r in ['index.json'] + list(files))
s = s.replace('<qresource prefix="/data">', '<qresource prefix="/data">' + add, 1)
open(q, 'w').write(s)
print(len(files), 'files,', len(ids), 'tools, serial', serial)
