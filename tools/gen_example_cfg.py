#!/usr/bin/env python3
"""
gen_example_cfg.py - writes docs/ARIA_Settings.example.cfg from ARIA_Settings.c.

Mirrors ARIA_ConfigVisitor in SAVE mode (same line format the mod writes to
$profile:ARIA/ARIA_Settings.cfg on first start), so the documentation can never
drift from the code.

Usage: python3 tools/gen_example_cfg.py
"""
import os
import re

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SRC = os.path.join(ROOT, 'Scripts/Game/ARIA/Core/ARIA_Settings.c')
OUT = os.path.join(ROOT, 'docs/ARIA_Settings.example.cfg')

src = open(SRC, encoding='utf-8').read()

# class name -> {field: default literal}
defaults = {}
for cm in re.finditer(r'class (\w+)\s*\{(.*?)\n\}', src, re.S):
    fields = {}
    for fm in re.finditer(r'^\s*(?:float|int|bool|string)\s+(m_\w+)\s*=\s*([^;]+);', cm.group(2), re.M):
        fields[fm.group(1)] = fm.group(2).strip()
    defaults[cm.group(1)] = (fields, cm.group(2))

ENUMS = {'ARIA_ELogLevel.INFO': '2', 'ARIA_EFlightModel.VELOCITY': '0'}


def fmt(value, kind):
    value = ENUMS.get(value, value)
    if kind == 'Bool':
        return value
    if kind == 'Str':
        return value.strip('"')
    if kind == 'Float':
        v = float(value)
        return ('%g' % v)
    return str(int(float(value)))


def visit(cls, prefix_override=None, lines=None, comment_values=False):
    fields, body = defaults[cls]
    m = re.search(r'void Visit\(ARIA_ConfigVisitor v\)\s*\{(.*?)\n\t\}', body, re.S)
    prefix = ''
    for raw in m.group(1).split('\n'):
        line = raw.strip()
        bs = re.match(r'v\.BeginSection\("([^"]*)"(?:\s*\+\s*m_sFactionKey)?,\s*(.*)\);', line)
        if bs:
            prefix = prefix_override or bs.group(1)
            title = bs.group(2)
            title = re.sub(r'"\s*\+\s*VERSION\s*\+\s*"', '1', title)
            title = title.replace('" + m_sFactionKey', ' default"').strip('"')
            lines += ['', '#' + '-' * 78, '# ' + title, '#' + '-' * 78]
            continue
        cm = re.match(r'v\.Comment\("(.*)"\);', line)
        if cm:
            lines.append('# ' + cm.group(1))
            continue
        if line.startswith('v.SetCommentValues(true)'):
            comment_values = True
            continue
        vm = re.match(r'(m_\w+)\s*=\s*v\.(Float|Int|Bool|Str)\("([^"]+)",\s*\1,\s*(.*)\);', line)
        if vm:
            field, kind, key, rest = vm.groups()
            args = [a.strip() for a in re.findall(r'"[^"]*"|[^,]+', rest)]
            desc = args[-1].strip('"')
            value = fmt(fields[field], kind)
            if kind in ('Float', 'Int'):
                desc = f'{desc} ({args[0]}..{args[1]})'
            elif kind == 'Bool':
                desc += ' (true/false)'
            text = f'{prefix}.{key} = {value}' if prefix else f'{key} = {value}'
            if comment_values:
                text = '#' + text
            lines.append(f'{text}    # {desc}')
            continue
        sub = re.match(r'(m_\w+)\.Visit\(v\);', line)
        if sub:
            cls_map = {'m_Squad': 'ARIA_SquadSettings', 'm_Vehicles': 'ARIA_VehicleSettings',
                       'm_Heli': 'ARIA_HeliSettings', 'm_DefaultSkill': 'ARIA_SkillProfile'}
            visit(cls_map[sub.group(1)], 'skill.default' if sub.group(1) == 'm_DefaultSkill' else None,
                  lines, comment_values)
            continue
        if line.startswith('VisitFactionSkills'):
            lines += ['# ', "# Per-faction overrides: copy the 'skill.default' block and replace 'default' with a faction key,",
                      "# for example 'skill.USSR.skill = 0.8'. Unlisted values inherit from skill.default."]
    return lines


lines = visit('ARIA_Settings', lines=[])
os.makedirs(os.path.dirname(OUT), exist_ok=True)
with open(OUT, 'w', encoding='utf-8') as f:
    f.write('\n'.join(lines).lstrip('\n') + '\n')
print(f'wrote {OUT} ({len(lines)} lines)')
