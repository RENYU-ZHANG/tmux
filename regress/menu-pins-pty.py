#!/usr/bin/env python3
"""Private native pin/hover, keyboard, nested and replacement acceptance."""
import argparse
import json
import os
from pathlib import Path
import tempfile
from agent_sessions.screen_capture import PrivateServer

p = argparse.ArgumentParser()
p.add_argument('binary', type=lambda v: str(Path(v).resolve()))
p.add_argument('evidence', type=Path)
p.add_argument('--term', default='xterm-256color')
p.add_argument('--ascii', action='store_true')
p.add_argument('--no-reopen', action='store_true', help='Verify the menu-only profile without optional right-press replacement')
a = p.parse_args()
a.evidence.mkdir(parents=True, exist_ok=True)
os.environ.pop('TMUX', None)
os.environ.pop('TMUX_PANE', None)

def snap(c, name):
    s = c.snapshot()
    (a.evidence / (name + '.html')).write_text(s.html())
    (a.evidence / (name + '.json')).write_text(json.dumps({'rows': s.rows}))
    return s

def pos(c, label):
    for y, row in enumerate(c.snapshot().rows):
        if label in row:
            return row.index(label) + 1, y + 1
    raise AssertionError((label, c.snapshot().rows))

def point(c, label, action='move'):
    c.mouse(action, 'left', *pos(c, label))
    if action == 'press': c.mouse('release', 'left', *pos(c, label))

def bg(c, label):
    x, y = pos(c, label)
    return next(cell.attributes.bg for yy, xx, cell in c.snapshot().cells() if yy == y-1 and xx == x-1)

def pinned(c, label):
    x, y = pos(c, label)
    return c.snapshot().rows[y-1][x-2] == ('*' if a.ascii else '◆')

with tempfile.TemporaryDirectory(prefix='menu-pins-') as d:
    with PrivateServer(tmux=a.binary, root=Path(d)) as s:
        s.environment['TERM'] = a.term
        s.environment['LC_ALL'] = 'C' if a.ascii else 'C.UTF-8'
        s.new_session('pins', 'cat', width=80, height=24)
        s.run('set', '-g', 'status', 'off')
        s.run('set', '-g', 'mouse', 'on')
        s.run('set', '-g', 'menu-selected-style', 'fg=black,bg=yellow')
        s.run('set', '-g', 'menu-hover-style', 'fg=black,bg=cyan')
        items = ['ParentA','','--submenu','ChildA','','--submenu',
                 'DeepLeaf','','set -g @chosen deep','--end',
                 'ChildLeaf','','set -g @chosen child','--end',
                 'ParentB','','--submenu','ChildB','','set -g @chosen b','--end',
                 'RootLeaf','','set -g @chosen root']
        s.run('bind', '-n', 'F11', 'display-menu', '-L', '-N', *([] if a.no_reopen else ['-R']), '-M', '-x', '15', '-y', '7', *items)
        if not a.no_reopen: s.run('bind', '-n', 'MouseDown3Pane', 'display-menu', '-L', '-N', '-R', '-x', 'M', '-y', 'M', 'Fresh','','set -g @chosen fresh')
        with s.client('pins', width=80, height=24) as c:
            c.key('f11')
            point(c, 'RootLeaf'); assert bg(c,'RootLeaf') == '3'
            c.mouse('move','left',1,1); assert bg(c,'RootLeaf') != '3'
            point(c,'ParentA'); point(c,'ChildA'); point(c,'DeepLeaf')
            assert bg(c,'ParentA') == bg(c,'ChildA') == bg(c,'DeepLeaf') == '3'
            snap(c,'traversal')
            point(c,'ParentA','press'); assert pinned(c,'ParentA')
            label_x = pos(c,'ParentA')[0]
            point(c,'ParentB'); assert pinned(c,'ParentA') and bg(c,'ParentB') == '6'
            assert pos(c,'ChildA') and all('ChildB' not in row for row in c.snapshot().rows)
            snap(c,'pin-sibling-hover')
            c.mouse('move','left',1,1); assert pinned(c,'ParentA') and bg(c,'ParentB') != '6'
            snap(c,'pin-pointer-outside')
            point(c,'ParentA'); assert bg(c,'ParentA') == '3' and pos(c,'ParentA')[0] == label_x
            point(c,'ParentA','press'); assert not pinned(c,'ParentA')
            point(c,'ParentB'); assert pos(c,'ChildB')
            point(c,'ParentB','press'); assert pinned(c,'ParentB')
            point(c,'ParentA','press'); assert pinned(c,'ParentA') and not pinned(c,'ParentB')
            point(c,'ChildA','press'); assert pinned(c,'ChildA')
            point(c,'ChildLeaf'); assert pinned(c,'ChildA') and bg(c,'ChildLeaf') == '6'
            snap(c,'nested-pins')
            if not a.no_reopen:
                c.mouse('press','right',*pos(c,'DeepLeaf'))
                assert not s.run('show','-gqv','@chosen').stdout.strip()
                snap(c,'replacement')
                assert all('ParentA' not in row and '◆' not in row for row in c.snapshot().rows)
                assert pos(c,'Fresh')
            c.key('escape'); c.key('f11')
            point(c,'ParentA','press'); c.key('down'); c.key('enter')
            assert pos(c,'DeepLeaf')
            c.key('left'); c.key('down'); c.key('enter')
            assert s.run('show','-gqv','@chosen').stdout.strip() == 'child'
            s.run('set','-gu','@chosen')
            c.key('f11'); point(c,'ParentA','press')
            c.mouse('press','left',1,1)
            assert not s.run('show','-gqv','@chosen').stdout.strip()
            assert all('ParentA' not in row for row in c.snapshot().rows)
print('PASS', a.term)
