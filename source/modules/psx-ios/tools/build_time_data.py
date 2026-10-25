"""Build local IOS geographic timezone/TZif data. No Python is used by IOS.
Inputs: timezone-boundary-builder full with-oceans GeoJSON zip and tzdata wheel.
No geometry simplification: coordinates are converted to IEEE float only.
"""
import json, zipfile, struct, hashlib, pathlib, sys
boundary, wheel, destination = map(pathlib.Path, sys.argv[1:])
with zipfile.ZipFile(boundary) as z:
    geo = json.loads(z.read(next(n for n in z.namelist() if n.endswith('.json') or n.endswith('.geojson'))))
names = sorted({f['properties']['tzid'] for f in geo['features']})
destination.parent.mkdir(parents=True, exist_ok=True)
with zipfile.ZipFile(wheel) as z, destination.open('wb') as out:
    version = z.read('tzdata/zoneinfo/tzdata.zi').splitlines()[0].decode()
    if version != '# version 2026d': raise ValueError(version)
    def u(n): out.write(struct.pack('<I', n))
    def text(s): b=s.encode(); u(len(b)); out.write(b)
    out.write(b'IOSTZ001'); text('TBB 2026d / IANA 2026d'); u(len(names))
    for name in names:
        text(name); data=z.read('tzdata/zoneinfo/'+name); u(len(data)); out.write(data)
    polygons=[]
    for f in geo['features']:
        g=f['geometry']; coordinates=g['coordinates']
        for p in ([coordinates] if g['type']=='Polygon' else coordinates): polygons.append((names.index(f['properties']['tzid']),p))
    u(len(polygons))
    for zone, rings in polygons:
        u(zone); u(len(rings))
        for ring in rings:
            u(len(ring)); out.write(b''.join(struct.pack('<ff', *point[:2]) for point in ring))
    for name in z.namelist():
        if name.endswith('/LICENSE') or name.endswith('/LICENSE_APACHE'): (destination.parent / ('IANA-' + pathlib.Path(name).name)).write_bytes(z.read(name))
manifest={'inputs':[{'file':str(p),'bytes':p.stat().st_size,'sha256':hashlib.sha256(p.read_bytes()).hexdigest()} for p in [boundary,wheel]],'output':{'file':str(destination),'bytes':destination.stat().st_size,'sha256':hashlib.sha256(destination.read_bytes()).hexdigest()},'version':version,'polygons':len(polygons),'zones':len(names),'geometry':'Full rings including holes; float32 coordinates, no simplification'}
destination.with_suffix('.json').write_text(json.dumps(manifest,indent=2))
print(json.dumps(manifest,indent=2))
