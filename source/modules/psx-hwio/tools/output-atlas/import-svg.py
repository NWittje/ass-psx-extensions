"""Compile the human's layered cockpit areas and qualified native lamp locations.

Authoring only: no simulator, hardware, SVG script or external URL is executed.
The runtime uses qualified PNGs and a small bounded XML navigation index.
"""
import argparse
import base64
import hashlib
import json
import math
import pathlib
import re
import xml.etree.ElementTree as X

COORDS = ('left', 'top', 'right', 'bottom')


def box(element):
    return tuple(float(element.get(k)) for k in COORDS)


def attributes(bounds):
    return {k: format(v, '.12g') for k, v in zip(COORDS, bounds)}


def area(b):
    return max(0, b[2]-b[0])*max(0, b[3]-b[1])


def intersect(a, b):
    return max(a[0], b[0]), max(a[1], b[1]), min(a[2], b[2]), min(a[3], b[3])


def contains(a, b):
    return a[0] <= b[0] and a[1] <= b[1] and a[2] >= b[2] and a[3] >= b[3]


def union(boxes):
    return tuple(f(b[k] for b in boxes) for k, f in enumerate((min, min, max, max)))


def transformed_rectangle(shape, layer):
    """Flatten SVG affine transforms; the runtime admits axis-aligned cells only.

    SVG lists postmultiply matrices, so points traverse each list right-to-left,
    then the containing layer. Never execute embedded SVG scripts/resources.
    """
    x, y, w, h = (float(shape.get(k, 'nan')) for k in ('x', 'y', 'width', 'height'))
    if not all(map(math.isfinite, (x, y, w, h))) or w <= 0 or h <= 0:
        raise ValueError('Invalid SVG rectangle: '+shape.get('id', ''))
    points = [(x, y), (x+w, y), (x+w, y+h), (x, y+h)]
    for element in (shape, layer):
        transform = element.get('transform', '')
        operations = list(re.finditer(r'([A-Za-z]+)\s*\(([^()]*)\)', transform))
        if re.sub(r'([A-Za-z]+)\s*\(([^()]*)\)', '', transform).strip(' ,\t\r\n'):
            raise ValueError('Invalid SVG transform')
        for match in reversed(operations):
            values = [float(v) for v in re.split(r'[\s,]+', match[2].strip()) if v]
            if not all(map(math.isfinite, values)):
                raise ValueError('Nonfinite SVG transform')
            op = match[1]
            if op == 'translate' and len(values) in (1, 2):
                a, b = values[0], values[1] if len(values) == 2 else 0
                points = [(x+a, y+b) for x, y in points]
            elif op == 'scale' and len(values) in (1, 2):
                a, b = values[0], values[-1]
                points = [(x*a, y*b) for x, y in points]
            elif op == 'rotate' and len(values) in (1, 3):
                c, s = math.cos(math.radians(values[0])), math.sin(math.radians(values[0]))
                a, b = values[1:] if len(values) == 3 else (0, 0)
                points = [(a+c*(x-a)-s*(y-b), b+s*(x-a)+c*(y-b)) for x, y in points]
            elif op == 'matrix' and len(values) == 6:
                a, b, c, d, e, f = values
                points = [(a*x+c*y+e, b*x+d*y+f) for x, y in points]
            else:
                raise ValueError('Unsupported SVG transform: '+op)
    if not all(math.isfinite(v) for p in points for v in p):
        raise ValueError('Invalid transformed SVG rectangle')
    for a, b in zip(points, points[1:]+points[:1]):
        if abs(a[0]-b[0]) > 1e-7 and abs(a[1]-b[1]) > 1e-7:
            raise ValueError('SVG click areas must remain axis-aligned rectangles')
    bounds = min(p[0] for p in points), min(p[1] for p in points), max(p[0] for p in points), max(p[1] for p in points)
    if not area(bounds):
        raise ValueError('Degenerate transformed SVG rectangle')
    return bounds


def svg_areas(root, world, aliases=None, extensions=()):
    # Illustrator escapes leading digits and underscores in XML IDs. Decode
    # once, retaining the full suffix: the supplied file has three distinct 6_5s.
    decode = lambda s: re.sub(r'_x([0-9A-Fa-f]{2,6})_', lambda m: chr(int(m[1], 16)), s)
    view = tuple(map(float, root.get('viewBox', '').split()))
    if len(view) != 4 or view[2:] != (4400, 6350) or view[:2] != (0, 0):
        raise ValueError('Expected the original 4400 x 6350 reference canvas')
    result, identifiers = [], set()
    for layer in root:
        name = layer.get('id', '')
        if not re.fullmatch('[a-z]', name):
            continue  # embedded reference/Illustrator metadata are not geometry
        depth = ord(name)-ord('a')+1
        for shape in layer:
            if shape.tag.split('}')[-1] != 'rect':
                raise ValueError('Click areas must be rectangles: '+name)
            label = decode(shape.get('id', ''))
            match = re.fullmatch(r'(\d+(?:_\d+)*)_(.+)', (aliases or {}).get(label, label))
            if not match:
                raise ValueError('Missing numeric hierarchy prefix: '+label)
            prefix = tuple(map(int, match[1].split('_')))
            if len(prefix) != depth:
                raise ValueError('Layer depth disagrees with numeric hierarchy: '+label)
            identifier = 'svg.'+label.lower()
            if identifier in identifiers:
                raise ValueError('Duplicate SVG area: '+label)
            identifiers.add(identifier)
            x, y, right, bottom = transformed_rectangle(shape, layer)
            b = (world[0]+x*(world[2]-world[0])/view[2], world[1]+y*(world[3]-world[1])/view[3],
                 world[0]+right*(world[2]-world[0])/view[2], world[1]+bottom*(world[3]-world[1])/view[3])
            if not all(map(math.isfinite, b)) or not area(b) or not contains(world, b):
                raise ValueError('SVG area outside reference: '+label)
            result.append(dict(id=identifier, label=match[2].replace('_', ' '), source=label,
                               prefix=prefix, depth=depth, bounds=b, original_bounds=b))
    result.sort(key=lambda v: (v['depth'], v['prefix'], v['source']))
    by_source = {r['source']: r for r in result}
    for parent, child in extensions:
        if parent not in by_source or child not in by_source:
            raise ValueError('Unknown approved overview extension')
        p, c = by_source[parent], by_source[child]
        if p['prefix'] != c['prefix'][:-1]:
            raise ValueError('Overview extension must retain direct numeric ancestry')
        p['bounds'] = union([p['bounds'], c['bounds']])
    def enclosed(parent, child):
        coverage = area(intersect(parent, child))/area(child)
        overhang = max((parent[0]-child[0])*view[2]/(world[2]-world[0]),
                       (parent[1]-child[1])*view[3]/(world[3]-world[1]),
                       (child[2]-parent[2])*view[2]/(world[2]-world[0]),
                       (child[3]-parent[3])*view[3]/(world[3]-world[1]), 0)
        return coverage >= .9 or (coverage >= .85 and overhang <= 3)
    for region in result:
        if region['depth'] == 1:
            region['parent'] = 'flightdeck'
        else:
            # Preserve the previous >=90% rule. The final drawing also offsets
            # three lower cells by 1.3px sideways/2.16px downward: admit only a
            # <=3 reference-pixel overhang with >=85% enclosure, then clip it.
            # Numeric ancestry and unique parent selection remain mandatory.
            candidates = [v for v in result if v['prefix'] == region['prefix'][:-1]
                          and enclosed(v['bounds'], region['bounds'])]
            if len(candidates) != 1:
                raise ValueError('Ambiguous or absent enclosing SVG parent: '+region['source'])
            region['parent'] = candidates[0]['id']
            region['bounds'] = intersect(region['bounds'], candidates[0]['bounds'])
    if not result or len(result) > 1024:
        raise ValueError('SVG area inventory outside bounds')
    return result


def qualify_references(svg, locations):
    references = [e for e in svg.iter() if e.tag.split('}')[-1] == 'image']
    expected = {locations.get('reference-png-sha256')}
    if locations.get('companion-png-sha256'):
        expected.add(locations.get('companion-png-sha256'))
    observed = []
    for reference in references:
        href = reference.get('{http://www.w3.org/1999/xlink}href', reference.get('href', ''))
        if reference.get('width') != '4400' or reference.get('height') != '6350' or not href.startswith('data:image/png;base64,'):
            raise ValueError('Unqualified SVG reference dimensions or source')
        observed.append(hashlib.sha256(base64.b64decode(href.split(',', 1)[1], validate=True)).hexdigest())
    required = locations.get('companion-png-sha256') or locations.get('reference-png-sha256')
    if not observed or len(observed) != len(set(observed)) or not set(observed) <= expected or required not in observed:
        raise ValueError('The SVG references differ from the qualified runtime PNGs')
    return observed


def compile_atlas(svg_file, locations_file, catalogue_file, links_file=None):
    locations = X.parse(locations_file).getroot()
    images = {e.get('id'): e for e in locations.findall('image')}
    world = box(images['flightdeck'])
    overview = dict(id='flightdeck', label='Flightdeck', bounds=world, depth=0)
    svg = X.parse(svg_file).getroot()
    observed = qualify_references(svg, locations)
    links_file = links_file or pathlib.Path(__file__).with_name('svg-links.xml')
    links = X.parse(links_file).getroot() if links_file.exists() else X.Element('svg-links')
    aliases = {e.get('source'): e.get('hierarchy') for e in links.findall('alias')}
    if links.tag != 'svg-links' or len(aliases) != len(links.findall('alias')):
        raise ValueError('Invalid or duplicate SVG hierarchy aliases')
    extensions = [(e.get('area'), e.get('include')) for e in links.findall('extend')]
    if any(not e.get('evidence') for e in links.findall('extend')):
        raise ValueError('Overview extension needs explicit evidence')
    regions = svg_areas(svg, world, aliases, extensions)
    by_source = {v['source']: v for v in regions}
    # Reuse confirmed repairs for older drawings, and retire each one when the
    # human fixes its full label. An unrelated rename still needs explicit review.
    retired_aliases = {old: new for old, new in aliases.items() if old not in by_source and new in by_source}
    if set(aliases)-set(by_source)-set(retired_aliases):
        raise ValueError('Obsolete SVG hierarchy alias; review the changed document')
    captions = {e.get('area'): e.get('label') for e in links.findall('caption')}
    if len(captions) != len(links.findall('caption')) or set(captions)-set(by_source) or any(not v for v in captions.values()):
        raise ValueError('Invalid SVG caption correction')
    for source, label in captions.items():
        by_source[source]['label'] = label
    qualified = {e.get('id'): e for e in X.parse(catalogue_file).iter('digital-output')
                 if e.get('qualified') == 'true' and e.get('kind') in ('lamp', 'shutter')}
    native = {e.get('output'): e for e in locations.findall('location')}
    if len(native) != len(locations.findall('location')) or set(native) != set(qualified):
        raise ValueError('Native location evidence must exactly cover the independent qualified catalogue')
    excluded_images = {e.get('image'): e.get('reason') for e in links.findall('exclude')}
    if len(excluded_images) != len(links.findall('exclude')) or set(excluded_images)-set(images) or any(not v for v in excluded_images.values()):
        raise ValueError('Invalid module-output exclusion')
    excluded = {k: excluded_images[n.get('image')] for k, n in native.items() if n.get('image') in excluded_images}
    excluded_areas = {e.get('source') for e in links.findall('exclude-area')}
    if len(excluded_areas) != len(links.findall('exclude-area')) or excluded_areas-set(by_source):
        raise ValueError('Unknown module SVG area')
    removed = {by_source[k]['id'] for k in excluded_areas}
    for r in regions:
        if r['parent'] in removed:
            removed.add(r['id'])
    regions = [r for r in regions if r['id'] not in removed]
    refinements = {e.get('output'): e for e in links.findall('legend')}
    if len(refinements) != len(links.findall('legend')) or set(refinements)-set(native):
        raise ValueError('Invalid SVG legend qualification')
    fallbacks = {e.get('output'): e for e in links.findall('fallback')}
    if len(fallbacks) != len(links.findall('fallback')) or set(fallbacks)-set(native):
        raise ValueError('Invalid optional-layout placement')
    placements, deferred, clipped, refined = {}, [], [], []
    for identifier, n in native.items():
        if identifier in excluded:
            continue
        b = box(n)
        if identifier in refinements:
            evidence = refinements[identifier]
            side = evidence.get('side')
            if side not in ('upper', 'lower', 'left', 'right') or not evidence.get('evidence'):
                raise ValueError('Missing native legend-side evidence: '+identifier)
            middle = (b[1]+b[3])/2 if side in ('upper', 'lower') else (b[0]+b[2])/2
            b = (b[0], b[1], b[2], middle) if side == 'upper' else (b[0], middle, b[2], b[3]) if side == 'lower' else (b[0], b[1], middle, b[3]) if side == 'left' else (middle, b[1], b[2], b[3])
            refined.append(identifier)
        # A majority of the actual illuminated geometry must lie in an area.
        # Do not silently put a nearby lamp in a panel based on its name alone.
        if identifier in fallbacks:
            f = fallbacks[identifier]
            target = overview if f.get('area') == 'flightdeck' else by_source.get(f.get('area'))
            if not target or not f.get('evidence') or not contains(target['bounds'], b):
                raise ValueError('Optional-layout placement has no qualified enclosing area: '+identifier)
            candidates = [target]
        else:
            candidates = [v for v in regions if area(intersect(b, v['bounds'])) >= area(b)*.5]
        if not candidates:
            deferred.append(identifier)
            continue
        depth = max(v['depth'] for v in candidates)
        candidates = [v for v in candidates if v['depth'] == depth]
        if len(candidates) != 1:
            raise ValueError('Overlapping same-depth areas for '+identifier)
        r = candidates[0]
        placement = intersect(b, r['bounds'])
        if placement != b:
            clipped.append(identifier)
        placements[identifier] = dict(region=r['id'], bounds=placement)

    atlas = X.Element('output-atlas', version='2', complete='false' if deferred or excluded else 'true')
    used_images = {'flightdeck'} | {native[k].get('image') for k in placements}
    for identifier in sorted(used_images):
        X.SubElement(atlas, 'image', images[identifier].attrib)
    X.SubElement(atlas, 'region', id='flightdeck', parent='', image='flightdeck', output='',
                 label='Flightdeck', **attributes(world))
    output_regions = {}
    output_regions['flightdeck'] = atlas.findall('region')[0]
    def caption(k):
        context = native[k].get('context')
        return qualified[k].get('name')+(' ('+context+')' if context else '')
    for region in regions:
        # Reuse the native panel reference at deep zoom levels. The overview
        # supplies navigation, not enough pixels to read a single PBA caption.
        contained = [images[native[k].get('image')] for k, p in placements.items()
                     if contains(region['bounds'], p['bounds']) and contains(box(images[native[k].get('image')]), region['bounds'])]
        image = min(contained, key=lambda e: area(box(e))).get('id') if contained else 'flightdeck'
        e = X.SubElement(atlas, 'region', id=region['id'], parent=region['parent'], image=image,
                         output='', label=region['label'], **attributes(region['bounds']))
        output_regions[region['id']] = e
    for region in [overview]+regions:
        identifiers = [k for k, p in placements.items() if p['region'] == region['id']]
        subareas = any(v['parent'] == region['id'] for v in regions)
        if len(identifiers) == 1 and not subareas:
            # A labelled single indicator is already a selectable leaf. Do not
            # add redundant click levels merely to match the deepest panel.
            output_regions[region['id']].set('output', identifiers[0])
            output_regions[region['id']].set('label', region['label']+' — '+caption(identifiers[0]))
            continue
        controls = sorted({native[k].get('control') for k in identifiers},
                          key=lambda k: (min(box(native[n])[1] for n in identifiers if native[n].get('control') == k),
                                         min(box(native[n])[0] for n in identifiers if native[n].get('control') == k), k))
        for control in controls:
            lamps = sorted((k for k in identifiers if native[k].get('control') == control),
                           key=lambda k: (box(native[k])[1], box(native[k])[0], k))
            parent = region['id']
            if len(lamps) > 1:
                parent += '.'+control
                label = ' / '.join(caption(k) for k in lamps)
                if len(label.encode('utf-8')) > 240:
                    label = native[lamps[0]].get('control-label')
                image_ids = {native[k].get('image') for k in lamps}
                image = next(iter(image_ids)) if len(image_ids) == 1 else 'flightdeck'
                X.SubElement(atlas, 'region', id=parent, parent=region['id'], image=image,
                             output='', label=label, **attributes(union([placements[k]['bounds'] for k in lamps])))
            for k in lamps:
                n = native[k]
                X.SubElement(atlas, 'region', id=k, parent=parent, image=n.get('image'), output=k,
                             label=caption(k), **attributes(placements[k]['bounds']))
    for k in sorted(deferred):
        X.SubElement(atlas, 'deferred', output=k, reason='No supplied SVG area covers this indicator')
    for k, reason in sorted(excluded.items()):
        X.SubElement(atlas, 'deferred', output=k, reason=reason)
    referenced_images = {e.get('image') for e in atlas.findall('region')}
    for e in list(atlas.findall('image')):
        if e.get('id') not in referenced_images:
            atlas.remove(e)
    X.indent(atlas)
    report = dict(svg_sha256=hashlib.sha256(svg_file.read_bytes()).hexdigest(),
                  native_locations_sha256=hashlib.sha256(locations_file.read_bytes()).hexdigest(),
                  qualified=len(qualified), placed=len(placements), deferred=sorted(deferred), clipped=clipped,
                  excluded=excluded, excluded_areas=sorted(excluded_areas), corrected_captions=captions,
                  reference_hashes=observed,
                  root_outputs=sorted(k for k, p in placements.items() if p['region'] == 'flightdeck'),
                  links_sha256=hashlib.sha256(links_file.read_bytes()).hexdigest() if links_file.exists() else None,
                  applied_aliases={k: v for k, v in aliases.items() if k in by_source}, retired_aliases=retired_aliases,
                  optional_layout_fallbacks=sorted(fallbacks),
                  refined=refined, clipped_areas=[v['source'] for v in regions if v['bounds'] != v['original_bounds'] and area(v['bounds']) <= area(v['original_bounds'])],
                  extended_areas=[v['source'] for v in regions if area(v['bounds']) > area(v['original_bounds'])],
                  areas=[dict(id=v['id'], source=v['source'], parent=v['parent'], bounds=v['bounds'], depth=v['depth'],
                              outputs=[dict(id=k, name=qualified[k].get('name'),
                                            records=sorted({o.get('record') for o in qualified[k].iter('operand')}))
                                       for k, p in placements.items() if p['region'] == v['id']]) for v in regions])
    return atlas, report


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ('svg', 'locations', 'catalogue', 'atlas', 'report'):
        parser.add_argument(name, type=pathlib.Path)
    a = parser.parse_args()
    root, report = compile_atlas(a.svg, a.locations, a.catalogue)
    with a.atlas.open('xb') as stream:
        X.ElementTree(root).write(stream, encoding='utf-8', xml_declaration=True)
    with a.report.open('x', encoding='utf-8') as stream:
        json.dump(report, stream, indent=2)
        stream.write('\n')
    print(json.dumps({k: report[k] for k in ('qualified', 'placed', 'deferred', 'clipped')}))
