"""WP-U2 evidence: count sidecar submaterials by shader family x MtlFlags&0x8000 x Emissive>0 x Glow>0.
Usage: python3 -I ue/scripts/survey_materials.py <export root>"""
import json, glob, collections, sys
root = sys.argv[1]
c = collections.Counter(); names = collections.defaultdict(set); total = 0
for f in glob.glob(root + '/**/*.materials.json', recursive=True):
    for s in json.load(open(f))['submaterials']:
        a = {x['name']: x['value'] for x in s['authored_attributes']}
        fwd = bool(int(a.get('MtlFlags', '0')) & 0x8000)
        em = any(float(v) > 0 for v in a.get('Emissive', '0,0,0').split(','))
        gl = float(a.get('Glow', a.get('GlowAmount', '0')) or 0) > 0
        k = (s['shader_family'], 'forward' if fwd else '-', 'Emissive' if em else '-', 'Glow' if gl else '-')
        c[k] += 1; names[k].add(s['submaterial_name']); total += 1
print('total', total)
for k, v in sorted(c.items()):
    if k[1] == 'forward' or k[2] == 'Emissive' or k[3] == 'Glow':
        print(v, k, sorted(names[k])[:6])
