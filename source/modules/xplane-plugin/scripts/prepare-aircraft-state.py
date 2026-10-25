"""Generate the hash-qualified Sparky PSX adapter without changing the installation.

Vendor meshes/textures remain local. Only object references, six LE bindings,
door curves, read-only animation endpoints and powered-light adapters change.
"""
import argparse
import hashlib
import json
import re
from pathlib import Path

BASE_ACF = '19D499D63793C5492AC22158748C8E61DA1FF3675D3D686327DF94B161D48DE6'
PREFIX = 'psx_ios/aircraft_sync/'
GEAR_ORDER = [0, 4, 3, 2, 1]  # XP nose/bodyR/bodyL/wingR/wingL -> PXA1
LIGHT_MASK = 0x367f  # Native electrical brightness does not qualify an independent flash phase.
LIGHT_DEFINITIONS = {}
LIGHT_CELLS = {}
GUARD = '''-- PSX aircraft-state adapter v1: acknowledged exact-profile ownership only.
PSX_owned_ref = find_dataref("psx_ios/aircraft_sync/owned_groups")
PSX_owned_components = find_dataref("psx_ios/aircraft_sync/owned_components")
local function PSX_owned(group, component)
    if math.floor(PSX_owned_ref / (2 ^ group)) % 2 ~= 1 then return false end
    if component == nil then return true end
    return math.floor(PSX_owned_components[group] / (2 ^ component)) % 2 == 1
end
local function PSX_spoiler_component(i)
    if i <= 5 then return i - 1 end
    if i <= 7 then return 5 end
    return i - 2
end
local PSX_landing_components = {[0]=0, [1]=2, [2]=3, [3]=1}
local PSX_generic_components = {[0]=4, [1]=5, [2]=12, [3]=13}
'''


def digest(data):
    return hashlib.sha256(data).hexdigest().upper()


def uncomment(line, state):
    """Remove Lua comments for selecting complete assignment lines, never edit comments."""
    pos = 0
    result = ''
    while pos < len(line):
        if state:
            end = line.find(']]', pos)
            if end < 0:
                return result, True
            pos, state = end + 2, False
        else:
            begin = line.find('--', pos)
            if begin < 0:
                return result + line[pos:], False
            result += line[pos:begin]
            if line.startswith('--[[', begin):
                pos, state = begin + 4, True
            else:
                return result, False
    return result, state


def guard_lua(data):
    text = data.decode('utf-8')
    aliases, refs = {}, {}
    for var, ref in re.findall(r'(\w+)\s*=\s*(?:find_dataref|deferred_dataref)\("([^"]+)"', text):
        refs[var] = ref
        if re.fullmatch(r'laminar/B747/flt_ctrls/(?:rudder_[lu][wp]r_pos|[lr]_(?:aileron|elev)_(?:inner|outer))', ref):
            aliases[var] = 0
        elif ref == 'laminar/B747/flt_ctrls/spoilers':
            aliases[var] = 1
        elif ref == 'laminar/B747/flt_ctrls/flaps':
            aliases[var] = 2
        elif ref in ['sim/cockpit2/switches/landing_lights_switch', 'sim/cockpit2/switches/taxi_light_on',
                     'sim/cockpit/electrical/beacon_lights_on', 'sim/cockpit2/switches/beacon_on',
                     'sim/cockpit2/switches/navigation_lights_on', 'sim/cockpit2/switches/strobe_lights_on']:
            aliases[var] = 5
        elif ref == 'sim/cockpit2/switches/generic_lights_switch':
            aliases[var] = 5
    # The included hydraulics file shares the importing script's global aliases.
    aliases.update({v: 0 for v in ['B747DR_rudder_lwr_pos', 'B747DR_rudder_upr_pos',
                   'B747DR_l_elev_inner', 'B747DR_r_elev_inner', 'B747DR_l_elev_outer', 'B747DR_r_elev_outer',
                   'B747DR_l_aileron_inner', 'B747DR_r_aileron_inner', 'B747DR_l_aileron_outer', 'B747DR_r_aileron_outer']})
    aliases.update(B747DR_spoilers=1, B747DR_flaps=2)
    output, changes, state = [], [], False
    for number, line in enumerate(text.splitlines(keepends=True), 1):
        code, state = uncomment(line, state)
        match = re.fullmatch(r'\s*(\w+)(\[[^]]+\])?\s*=(?!=).+', code.strip())
        if match and match[1] in aliases and not re.search(r'(find_dataref|deferred_dataref|create_dataref)\(', code):
            var, index = match[1], match[2]
            # Preserve all unrelated generic lights, including variable-index unload clearing.
            component = None
            controls = {'B747DR_l_aileron_outer': 0, 'B747DR_l_aileron_inner': 1,
                        'B747DR_r_aileron_inner': 2, 'B747DR_r_aileron_outer': 3,
                        'B747DR_l_elev_outer': 4, 'B747DR_l_elev_inner': 5,
                        'B747DR_r_elev_inner': 6, 'B747DR_r_elev_outer': 7,
                        'B747DR_rudder_upr_pos': 8, 'B747DR_rudder_lwr_pos': 9}
            if var in controls:
                component = str(controls[var])
            elif var == 'B747DR_spoilers' and index:
                component = f'PSX_spoiler_component({index[1:-1]})'
            elif var == 'B747DR_flaps' and index:
                component = f'({index[1:-1]}) + 5'
            elif refs.get(var) == 'sim/cockpit2/switches/landing_lights_switch' and index:
                component = f'PSX_landing_components[{index[1:-1]}]'
            elif refs.get(var) == 'sim/cockpit2/switches/taxi_light_on':
                component = '6'
            elif refs.get(var) == 'sim/cockpit2/switches/strobe_lights_on':
                component = '11'
            condition = f'not PSX_owned({aliases[var]}' + (', ' + component if component is not None else '') + ')'
            if refs.get(var) in ['sim/cockpit/electrical/beacon_lights_on', 'sim/cockpit2/switches/beacon_on']:
                condition = 'not (PSX_owned(5, 7) or PSX_owned(5, 8))'
            if refs.get(var) == 'sim/cockpit2/switches/navigation_lights_on':
                condition = 'not (PSX_owned(5, 9) or PSX_owned(5, 10))'
            if 'generic' in var.lower():
                if not index:
                    raise ValueError('unexpected whole generic-light assignment')
                condition = f'({index[1:-1]}) > 3 or not PSX_owned(5, PSX_generic_components[{index[1:-1]}])'
            if '\n' not in line:
                ending = '\n'
            else:
                ending = '\r\n' if line.endswith('\r\n') else '\n'
            indent = re.match(r'\s*', line)[0]
            output += [indent + 'if ' + condition + ' then' + ending, line, indent + 'end' + ending]
            changes.append({'line': number, 'group': aliases[var], 'statement': code.strip()})
        else:
            output.append(line)
    if not changes:
        return data, []
    return (GUARD.replace('\n', '\r\n') + ''.join(output)).encode('utf-8'), changes


def lamp_component(name, fields):
    if name not in LIGHT_DEFINITIONS or len(fields) != 3 + LIGHT_DEFINITIONS[name]:
        return None  # Preserve malformed/unknown native recipes, never guess a lamp index.
    if name.startswith('airplane_landing_'):
        index = int(float(fields[6])) if name.endswith('_sp') else int(float(fields[6]))
        return [0, 2, 3, 1][index]
    if name.startswith('airplane_taxi_'):
        return 6
    if name.startswith('airplane_generic_'):
        raw = float(fields[6])
        if raw != int(raw):
            return None
        index = int(raw)
        return {0: 4, 1: 5, 2: 12, 3: 13}.get(index)
    if 'beacon' in name:
        return 8 if float(fields[-1]) == 0 else 7
    if name.startswith('airplane_nav_left'):
        return 9
    if name.startswith('airplane_nav_right'):
        return 10
    if name.startswith('airplane_nav_tail'):
        return 9 if float(fields[0]) < 0 else 10
    if 'strobe' in name:
        return 11
    return None


def custom_lamp(tokens):
    name, p = tokens[1], tokens[2:]
    component = lamp_component(name, p)
    if component is None or not LIGHT_MASK & (1 << component):
        return None, ''
    xyz, ref = ' '.join(p[:3]), PREFIX + f'light_spill_{component}'
    if name.endswith('_sp') and len(p) == 9:
        return component, f'LIGHT_SPILL_CUSTOM {xyz} {" ".join(p[3:6])} 1 {p[7]} 0 0 -1 {p[8]} {ref}\r\n'
    if name not in LIGHT_CELLS:
        return None, ''
    cell, x, y = LIGHT_CELLS[name]
    size = p[3] if 'nav_' in name else p[7]
    # Local profile approximation: powered binary lamp, installed atlas cell/size/position.
    # Directional bloom/intensity appearance remains a loaded qualification target.
    color = '1 0 0' if 'nav_left' in name else '0 1 0' if 'nav_right' in name else '1 1 1'
    # Installed atlas: 1024x512, base64-pixel cells; coordinates index cells of this size.
    # Laminar's format example uses size2 (128px), giving an8x4 grid.
    uv = f'{x*cell/16:.9f} {y*cell/8:.9f} {(x+1)*cell/16:.9f} {(y+1)*cell/8:.9f}'
    return component, f'LIGHT_CUSTOM {xyz} {color} 1 {size} {uv} {ref}\r\n'


def graphics_lights(root, source_reader=None):
    output = ['I\r\n800\r\nOBJ\r\n',
              'TEXTURE ../../../../Resources/bitmaps/world/lites/1000_lights_close.png\r\n',
              'POINT_COUNTS 0 0 0 0\r\n']
    count = 0
    for name, hide in [('Exterior_Fuselage.obj', 'laminar/B747/objects/hideFuse'),
                       ('Exterior_Wings2.obj', None), ('Exterior_HStab.obj', 'laminar/B747/objects/hideHStab')]:
        # Preserve the complete original animation tree, without copying mesh/texture art.
        output.append('ANIM_begin\r\n')
        if hide:
            output.append(f'ANIM_hide 1 1 {hide}\r\n')
        depth = 0
        source = (source_reader(Path('objects/747_exterior') / name) if source_reader
                  else (root / 'objects/747_exterior' / name).read_bytes())
        animation_source = b''.join(l for l in source.splitlines(keepends=True) if not l.strip().startswith(b'LIGHT_PARAM'))
        animation_source, _ = object_adapter(name, animation_source)
        # Pair original lamps with adapted transforms so gear/LE proxies agree with meshes.
        adapted_anim = iter(l.strip() for l in animation_source.decode('utf-8').splitlines() if l.strip().startswith('ANIM_'))
        for line in source.decode('utf-8').splitlines():
            code = line.strip()
            if code.startswith('ANIM_'):
                output.append(next(adapted_anim) + '\r\n')
                depth += (code == 'ANIM_begin') - (code == 'ANIM_end')
                if depth < 0:
                    raise ValueError('unbalanced source light animation')
            elif code.startswith('LIGHT_PARAM ' ) or code.startswith('LIGHT_PARAM\t'):
                _, custom = custom_lamp(code.split())
                if custom:
                    output.append(custom)
                    count += 1
        if depth:
            raise ValueError('unbalanced source light animation')
        output.append('ANIM_end\r\n')
    if not count:
        raise ValueError('no qualified graphics lights')
    return ''.join(output).encode('utf-8'), count


def object_adapter(name, data):
    lines = data.decode('utf-8').splitlines(keepends=True)
    changes = []
    le_block, current_le = 0, None
    for i, line in enumerate(lines):
        code = line.strip()
        if code.startswith('#'):
            continue
        new = line
        # Only six explicit LE animation sections; translations and rotations share each slot.
        if 'controls/slat' in line:
            if code.startswith('ANIM_trans'):
                current_le = [5, 4, 0, 1, 2, 3][le_block]
                le_block += 1
            if current_le is None:
                raise ValueError('slat rotation before its translation')
            new = re.sub(r'sim/flightmodel2/controls/slat[12]_deploy_ratio', PREFIX + f'leading_edge_{current_le}', new)
        if 'gear/deploy_ratio' in line:
            native = int(re.search(r'deploy_ratio\[(\d)\]', line)[1])
            canonical = GEAR_ORDER[native]
            cyclic = name == 'Exterior_Fuselage.obj' and code.startswith('ANIM_rotate_begin') and all(
                'ANIM_rotate_key' in lines[i + k] for k in range(1, 5)) and float(lines[i + 1].split()[-1]) == float(lines[i + 4].split()[-1])
            suffix = f'door_{canonical}' if cyclic else f'gear_{canonical}'
            new = new.replace(f'sim/flightmodel2/gear/deploy_ratio[{native}]', PREFIX + suffix)
            if cyclic:
                maximum = float(lines[i + 2].split()[-1])
                for k in range(1, 5):
                    indent = re.match(r'\s*', lines[i + k])[0]
                    fraction = (k - 1) / 3
                    lines[i + k] = f'{indent}ANIM_rotate_key {fraction:.9f} {maximum * fraction:.9f}\r\n'
        new = re.sub(r'sim/flightmodel2/engines/thrust_reverser_deploy_ratio\[(\d)\]',
                     lambda m: PREFIX + 'reverser_' + m[1], new)
        # Hide only the exact healthy component; the graphics-only OBJ owns its light.
        if code.startswith('LIGHT_PARAM'):
            tokens = code.split()
            component, custom = custom_lamp(tokens)
            if component is not None:
                active = PREFIX + f'active_light_{component}'
                native = f'ANIM_begin\r\nANIM_hide 1 1 {active}\r\n' + new + 'ANIM_end\r\n'
                new = native
        # Direct mesh illumination uses powered binary source instead of local bus brightness.
        new = re.sub(r'sim/flightmodel2/lights/landing_lights_brightness_ratio\[(\d)\]',
                     lambda m: PREFIX + f'light_{[0,2,3,1][int(m[1])]}', new)
        new = re.sub(r'sim/flightmodel2/lights/generic_lights_brightness_ratio\[([01])\]',
                     lambda m: PREFIX + f'light_{4+int(m[1])}', new)
        new = new.replace('sim/flightmodel2/lights/taxi_lights_brightness_ratio[0]', PREFIX + 'light_6')
        if new != line:
            changes.append({'line': i + 1, 'before': code, 'after': new.strip()})
        lines[i] = new
    if name == 'Exterior_Wings2.obj' and le_block != 6:
        raise ValueError('expected exactly six LE mesh sections')
    return ''.join(lines).encode('utf-8'), changes



def reconstruct_originals(root, output, expected_identity_header):
    """Invert only qualified prior edits; SHA-256, not resemblance, admits inputs."""
    root, output = root.resolve(), output.resolve()
    if not output.is_relative_to(Path(r'O:\psx\bin\.work')) or output.exists():
        raise ValueError('use a fresh owner-leased O original-input directory')
    expected = dict(re.findall(r'\{"([^"]+)","([A-F0-9]{64})"\}',
                              expected_identity_header.read_text(encoding='utf-8')))
    manifest_bytes = (root/'adapter-manifest.json').read_bytes()
    if digest(manifest_bytes) != expected['adapter-manifest.json']:
        raise ValueError('original reconstruction requires the compiled installed manifest')
    manifest=json.loads(manifest_bytes)
    installed_acf=(root/'747-400-PSX.acf').read_bytes()
    if digest(installed_acf) != expected['747-400-PSX.acf']:
        raise ValueError('unqualified installed adapted ACF')
    added=b''.join((line+'\n').encode() for line in manifest['acf_added_fields'])
    if installed_acf.count(added)!=1 or installed_acf.count(b'P _obja/count 42')!=1:
        raise ValueError('qualified ACF object-table inverse unavailable')
    base=installed_acf.replace(added,b'').replace(b'P _obja/count 42',b'P _obja/count 41')
    for entry in manifest['files']:
        if entry.get('source','').endswith('.obj') and not entry.get('generated'):
            base=base.replace(entry['target'].removeprefix('objects/').encode(),
                              entry['source'].removeprefix('objects/').encode())
    if digest(base)!=BASE_ACF:
        raise ValueError('reconstructed name-only ACF differs from fixed original identity')
    original,count=re.subn(rb'(?m)^(P acf/_name[^\r\n]*) PSX$',rb'\1',base)
    if count!=1 or digest(original)!=manifest['original_entry_sha256']:
        raise ValueError('reconstructed original ACF differs from manifest')
    files={Path('747-400.acf'):original,Path('747-400-PSX.acf'):base}
    for entry in manifest['files']:
        if not entry.get('source','').endswith('.lua'):
            continue
        relative=Path(entry['source'])
        if relative.is_absolute() or '..' in relative.parts:raise ValueError('unsafe original path')
        guarded=(root/relative).read_bytes()
        prefix=GUARD.replace('\n','\r\n').encode()
        if digest(guarded)!=entry['sha256'] or not guarded.startswith(prefix):
            raise ValueError('unqualified installed Lua input')
        lines=guarded[len(prefix):].splitlines(keepends=True)
        for index,change in reversed(list(enumerate(entry['changes']))):
            at=change['line']-1+2*index
            if b'PSX_owned' not in lines[at] or lines[at+2].strip()!=b'end':
                raise ValueError('qualified Lua wrapper inverse unavailable')
            del lines[at+2];del lines[at]
        candidate=b''.join(lines)
        if digest(candidate)!=entry['source_sha256'] or guard_lua(candidate)[0]!=guarded:
            raise ValueError('reconstructed Lua differs from exact original/forward result')
        files[relative]=candidate
    output.mkdir(parents=True)
    identities=[]
    for relative,data in files.items():
        target=output/relative;target.parent.mkdir(parents=True,exist_ok=True);target.write_bytes(data)
        if target.read_bytes()!=data:raise ValueError('reconstructed input readback differs')
        identities.append(dict(path=str(target),relative=relative.as_posix(),bytes=len(data),sha256=digest(data)))
    result=dict(InstalledManifestSHA256=digest(manifest_bytes),InstalledACFSHA256=digest(installed_acf),
                Inputs=identities,InstalledWrite=False,OriginalFailureEvidenceRecovered=False,
                Method='Exact inverse of qualified ACF/object-table/Lua edits; original hashes and forward Lua equality')
    (output/'reconstruction.json').write_bytes((json.dumps(result,indent=2)+'\n').encode())
    return result


def prepare(root, output, identity_header=None, expected_identity_header=None, qualified_originals=None):
    root, output = root.resolve(), output.resolve()
    if root == output or root in output.parents:
        raise ValueError('output must not be inside the installed aircraft')
    # A clean rebuild reads the original inputs, including the exact pre-install
    # Lua backups. Never use the damaged package or replace a compiled identity.
    originals = None
    expected = None
    if expected_identity_header:
        expected = dict(re.findall(r'\{"([^"]+)","([A-F0-9]{64})"\}',
                                   expected_identity_header.read_text(encoding='utf-8')))
        manifest_bytes = (root / 'adapter-manifest.json').read_bytes()
        if digest(manifest_bytes) != expected.get('adapter-manifest.json'):
            raise ValueError('installed manifest differs from compiled identity')
        originals = json.loads(manifest_bytes)
        original_acf = ((qualified_originals if qualified_originals else root) / '747-400.acf').read_bytes()
        if digest(original_acf) != originals['original_entry_sha256']:
            raise ValueError('original aircraft entry changed')
        acf, count = re.subn(rb'(?m)^(P acf/_name[^\r\n]*)', rb'\1 PSX', original_acf)
        if count != 1:
            raise ValueError('expected exactly one original aircraft name')
        cache = {Path('747-400.acf'): original_acf, Path('747-400-PSX.acf'): acf}
        backup = root / 'psx-state-backups' / digest(manifest_bytes)[:16]
        for entry in originals['files']:
            if entry.get('generated') or entry['source'] == '747-400-PSX.acf':
                continue
            relative = Path(entry['source'])
            if relative.is_absolute() or '..' in relative.parts:
                raise ValueError('unsafe original source path')
            candidate = qualified_originals / relative if qualified_originals else root / relative
            data = candidate.read_bytes() if candidate.exists() else (root / relative).read_bytes()
            if digest(data) != entry['source_sha256']:
                data = (backup / relative).read_bytes()
            if digest(data) != entry['source_sha256']:
                raise ValueError('unqualified original input: ' + str(relative))
            cache[relative] = data
        def read_source(relative):
            return cache[relative]
    else:
        acf = (root / '747-400-PSX.acf').read_bytes()
        def read_source(relative):
            return (root / relative).read_bytes()
    if digest(acf) != BASE_ACF:
        raise ValueError('unqualified input PSX ACF; preserve it and re-audit')
    definitions = (root.parents[1] / 'Resources/bitmaps/world/lites/lights.txt').read_bytes()
    for line in definitions.decode('utf-8').splitlines():
        p = line.split()
        if len(p) >= 3 and p[0] == 'LIGHT_PARAM_DEF':
            LIGHT_DEFINITIONS[p[1]] = int(p[2])
        if len(p) >= 10 and p[0] == 'BILLBOARD_SW':
            LIGHT_CELLS[p[1]] = tuple(map(int, p[7:10]))
    output.mkdir(parents=True, exist_ok=True)
    manifest = {'adapter': 1, 'base_acf_sha256': BASE_ACF, 'files': [], 'geometry_unchanged': True,
                'geometry_id': ['19D499D63793C549', '2AC22158748C8E61'], 'geometry_revision': 1,
                'capability_masks': [0x39f, 0x2f5, 0x3ff, 0x3ff, 15, LIGHT_MASK],
                'lights_txt_sha256': digest(definitions),
                'atlas_sha256': digest((root.parents[1] / 'Resources/bitmaps/world/lites/1000_lights_close.png').read_bytes()),
                'limits': ['Inner elevator slots5/6 have no independent OBJ consumer.',
                           'Stabilizer absolute incidence datum is not yet qualified; slot10 unavailable.',
                           'Beacon7/8 and strobe11 remain native: independent flash phase unavailable.',
                           'Spoiler slots1/3/8/10 have no OBJ consumer; native target2/4/10/12 remain native.',
                           'Lights and reversers are binary/profile approximations; loaded appearance unqualified.']}
    def save(relative, original, adapted, changes, target=None):
        target = target or relative
        path = output / target
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_bytes(adapted)
        manifest['files'].append({'source': relative.as_posix(), 'target': target.as_posix(),
                                 'source_sha256': digest(original), 'sha256': digest(adapted), 'changes': changes})
    object_paths = (sorted(root / relative for relative in cache if relative.suffix == '.obj')
                    if originals else sorted((root / 'objects/747_exterior').glob('*.obj')))
    for path in object_paths:
        original = read_source(path.relative_to(root))
        adapted, changes = object_adapter(path.name, original)
        if changes:
            target = path.relative_to(root).with_name(path.stem + '.PSX-state.obj')
            save(path.relative_to(root), original, adapted, changes, target)
            before = path.relative_to(root / 'objects').as_posix().encode()
            after = target.relative_to('objects').as_posix().encode()
            if before not in acf:
                raise ValueError('adapted object is not in the qualified PSX ACF: ' + path.name)
            acf = acf.replace(before, after)
    lua_paths = (sorted(root / relative for relative in cache if relative.suffix == '.lua')
                 if originals else sorted((root / 'plugins').glob('*/scripts/**/*.lua')))
    for path in lua_paths:
        original = read_source(path.relative_to(root))
        adapted, changes = guard_lua(original)
        if changes:
            save(path.relative_to(root), original, adapted, changes)
    original_acf = read_source(Path('747-400-PSX.acf'))
    graphics, light_count = graphics_lights(root, read_source)
    relative = Path('objects/747_exterior/PSX_State_Lights.obj')
    save(relative, b'', graphics, ['graphics-only; original animation trees and installed atlas reference'])
    manifest['files'][-1]['generated'] = True
    manifest['custom_light_count'] = light_count
    # Explicit approved object41 attach matches existing exterior objects20/21/23.
    # No physical geometry, original object fields or original entry change.
    added = []
    for line in original_acf.splitlines(keepends=True):
        if line.startswith(b'P _obja/20/'):
            line = line.replace(b'_obja/20/', b'_obja/41/')
            if b'/_v10_att_file_stl ' in line:
                line = b'P _obja/41/_v10_att_file_stl 747_exterior/PSX_State_Lights.obj\n'
            elif b'/_obj_hide_dataref ' in line:
                line = b'P _obja/41/_obj_hide_dataref \n'  # Per-source hiding lives in OBJ sections.
            added.append(line)
    if len(added) != 14 or acf.count(b'P _obja/count 41') != 1:
        raise ValueError('unexpected ACF object table')
    acf = acf.replace(b'P _obja/count 41', b''.join(added) + b'P _obja/count 42')
    manifest['acf_added_fields'] = [l.decode().rstrip('\r\n') for l in added]
    manifest['acf_count_change'] = [41, 42]
    before = [l for l in original_acf.splitlines() if b'/_v10_att_file_stl ' not in l and not l.startswith(b'P _obja/count ')]
    after = [l for l in acf.splitlines() if b'/_v10_att_file_stl ' not in l and not l.startswith((b'P _obja/count ', b'P _obja/41/'))]
    if before != after:
        raise ValueError('ACF edit outside exact object-path/object41/count allowlist rejected')
    save(Path('747-400-PSX.acf'), original_acf, acf, ['existing object paths plus exact graphics-object41/count allowlist'])
    manifest['acf_sha256'] = digest(acf)
    manifest['original_entry_sha256'] = digest(read_source(Path('747-400.acf')))
    (output / 'adapter-manifest.json').write_text(json.dumps(manifest, indent=2) + '\n', encoding='utf-8')
    if expected is not None:
        for relative, sha in expected.items():
            candidate = root / relative if relative.startswith('../../Resources/') else output / relative
            if digest(candidate.read_bytes()) != sha:
                raise ValueError('reconstructed successor differs from compiled identity: ' + relative)
    if identity_header:
        header = ['#pragma once', '#include <array>', '#include <string_view>', 'namespace psx::xp {',
                  'struct AircraftAdapterIdentity {std::string_view path,sha256;};',
                  f'inline constexpr std::string_view aircraft_adapter_acf_sha256="{manifest["acf_sha256"]}";',
                  f'inline constexpr std::array<AircraftAdapterIdentity,{len(manifest["files"])+3}> aircraft_adapter_files{{{{']
        for entry in manifest['files']:
            header.append('    {"' + entry['target'] + '","' + entry['sha256'] + '"},')
        header.append('    {"adapter-manifest.json","' + digest((output / 'adapter-manifest.json').read_bytes()) + '"},')
        header.append('    {"../../Resources/bitmaps/world/lites/lights.txt","' + manifest['lights_txt_sha256'] + '"},')
        header.append('    {"../../Resources/bitmaps/world/lites/1000_lights_close.png","' + manifest['atlas_sha256'] + '"},')
        header.extend(['}};', '}'])
        identity_header.parent.mkdir(parents=True, exist_ok=True)
        identity_header.write_text('\n'.join(header) + '\n', encoding='utf-8')
    print(json.dumps({'files': len(manifest['files']), 'acf_sha256': manifest['acf_sha256'], 'geometry_unchanged': True}))
    return manifest


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--aircraft', required=True, type=Path)
    parser.add_argument('--output', required=True, type=Path)
    parser.add_argument('--identity-header', type=Path)
    parser.add_argument('--expected-identity-header', type=Path,
                        help='Reconstruct from qualified originals/backups and verify every compiled identity; never update it')
    parser.add_argument('--qualified-originals', type=Path)
    parser.add_argument('--reconstruct-originals-to', type=Path)
    args = parser.parse_args()
    if args.identity_header and args.expected_identity_header:
        parser.error('verification cannot replace the expected identity header')
    if args.reconstruct_originals_to:
        if not args.expected_identity_header: parser.error('reconstruction requires compiled expected identities')
        print(json.dumps(reconstruct_originals(args.aircraft,args.reconstruct_originals_to,args.expected_identity_header)))
    else:
        prepare(args.aircraft, args.output, args.identity_header, args.expected_identity_header, args.qualified_originals)
