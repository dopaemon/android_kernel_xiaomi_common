#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0
#
# Generate <soc>-gpu-<variant>.dts: <soc>.dts with the Adreno power level
# table replaced by an undervolt and/or overclock table taken from KonaBess
# exports (konabess://<base64 gzip JSON>).
#
# usage (from arch/arm64/boot/dts/vendor/qcom):
#   ../tools/gen-gpu-profiles.py <soc> \
#       ../../../../../../include/dt-bindings/regulator/qcom,rpmh-regulator-levels.h . \
#       <variant>=<export> ...
#
#   cape:      uv=<UNDERVOLT> oc=<OC> uvoc=<UV+OC>
#   waipio-v2: uvoc=<UV+OC>
#   ukee:      uv=<UV> uvoc=<Balance> perf=<Perf> save=<Battery Save>
#
# Rules, so that only the GPU (GX) rail changes:
#  - qcom,level is rounded up to the next RPMh level (the GMU does the same
#    with values it does not know), never down;
#  - qcom,cx-level always comes from the stock level closest in frequency,
#    as KonaBess exports often drop it and the GMU then stops voting CX;
#    SoCs whose stock table has no CX votes (waipio) get none;
#  - bus indexes are clamped to the stock DDR table (kgsl does the same);
#  - uv keeps the stock frequencies, bus and ACD and only lowers each GX
#    level to the profile's level for that frequency (closest one if the
#    profile lacks it), never above stock;
#  - every other variant uses the profile's frequencies, GX levels, bus
#    and ACD.

import base64, gzip, json, re, sys

# soc: (base dts, stock pwrlevels, DDR bus table, KonaBess chip, GPU)
SOCS = {
    'cape': ('cape.dts', 'cape-gpu.dtsi', 'cape-gpu.dtsi', 'cape', 'Adreno 730'),
    'waipio-v2': ('waipio-v2.dts', 'waipio-v2-gpu.dtsi', 'waipio-gpu.dtsi', 'waipio', 'Adreno 730'),
    'ukee': ('ukee.dts', 'ukee.dtsi', 'cape-gpu.dtsi', 'ukee', 'Adreno 725'),
}

soc, levels_h, outdir = sys.argv[1:4]
base_dts, stock_dtsi, ddr_dtsi, chip, gpu = SOCS[soc]
profiles = dict(a.split('=', 1) for a in sys.argv[4:])

LEVELS = {}
for name, val in re.findall(r'#define\s+(RPMH_REGULATOR_LEVEL_\w+)\s+(\d+)', open(levels_h).read()):
    LEVELS[name] = int(val)
BY_VALUE = sorted((v, n) for n, v in LEVELS.items() if v < 0xffff)


def num(s):
    s = s.strip()
    return LEVELS[s] if s in LEVELS else int(s, 0)


def ceil_level(v):
    for val, name in BY_VALUE:
        if val >= v:
            return val, name
    sys.exit('level %d above every RPMh level' % v)


def parse_levels(text):
    out = []
    for body in re.findall(r'qcom,gpu-pwrlevel@\d+\s*\{(.*?)\};', text, re.S):
        g = lambda k: (re.search(r'\b' + k + r'\s*=\s*<([^>]*)>', body) or [None, None])[1]
        lv = {'freq': num(g('qcom,gpu-freq')), 'level': num(g('qcom,level'))}
        for k in ('bus-freq', 'bus-min', 'bus-max', 'cx-level'):
            v = g('qcom,' + k)
            lv[k] = num(v) if v is not None else None
        acd = g('qcom,acd-level')
        lv['acd'] = num(acd) if acd is not None else None
        out.append(lv)
    return out


stock_text = open(stock_dtsi).read()
stock = parse_levels(stock_text)
ddr = re.search(r'qcom,bus-table-ddr\s*=(.*?);', open(ddr_dtsi).read(), re.S).group(1)
bus_max = len(re.findall(r'<', ddr)) - 1
initial = int(re.search(r'qcom,initial-pwrlevel\s*=\s*<(\d+)>', stock_text).group(1))
initial_freq = stock[initial]['freq']


def nearest(table, freq):
    return min(table, key=lambda l: (abs(l['freq'] - freq), -l['freq']))


def load(path):
    s = open(path).read().strip()
    if not s.startswith('konabess://'):
        sys.exit('%s: not a KonaBess export' % path)
    d = json.loads(gzip.decompress(base64.b64decode(s[len('konabess://'):])))
    if not d.get('chip', '').startswith(chip):
        sys.exit('%s: profile is for %s, not %s' % (path, d.get('chip'), chip))
    return d.get('desc', ''), parse_levels(d['freq'])


def build(kind, prof):
    out = []
    if kind == 'uv':
        for s in stock:
            p = nearest(prof, s['freq'])
            lv = dict(s)
            lv['raw'] = p['level']
            lv['level'] = min(s['level'], ceil_level(p['level'])[0])
            out.append(lv)
        return out
    for p in prof:
        s = nearest(stock, p['freq'])
        bf = min(p['bus-freq'], bus_max)
        bmin = min(p['bus-min'] if p['bus-min'] is not None else bf, bus_max)
        bmax = min(p['bus-max'] if p['bus-max'] is not None else bf, bus_max)
        out.append({'freq': p['freq'], 'raw': p['level'], 'level': ceil_level(p['level'])[0],
                    'bus-freq': bf, 'bus-min': min(bmin, bmax), 'bus-max': bmax,
                    'acd': p['acd'] if p['acd'] is not None else s['acd'],
                    'cx-level': s['cx-level']})
    return out


TITLES = {
    'uv': 'undervolt (stock frequencies, lower GPU voltage)',
    'oc': 'overclock (higher maximum GPU frequency)',
    'uvoc': 'undervolt + overclock',
    'perf': 'undervolt + overclock with higher DDR votes',
    'save': 'undervolt + underclock (battery saving)',
}

for kind, path in profiles.items():
    desc, prof = load(path)
    table = build(kind, prof)
    init = table.index(nearest(table, initial_freq))
    name_of = lambda v: [n for val, n in BY_VALUE if val == v][0]
    L = ['/*',
         ' * DoraCore: %s with an %s %s' % (base_dts.replace('.dts', '.dtb'), gpu, TITLES[kind]),
         ' * power level table, from the KonaBess profile "%s".' % desc,
         ' * Generated by tools/gen-gpu-profiles.py; only the GPU (GX) rail',
         ' * levels change, CX votes stay as stock. Running the GPU outside the',
         ' * stock table may crash, freeze or damage the device.',
         ' */',
         '',
         '#include "%s"' % base_dts,
         '',
         '&msm_gpu {',
         '\t/delete-node/ qcom,gpu-pwrlevels;',
         '};',
         '',
         '&msm_gpu {',
         '\tqcom,initial-pwrlevel = <%d>;' % init,
         '',
         '\tqcom,gpu-pwrlevels {',
         '\t\t#address-cells = <1>;',
         '\t\t#size-cells = <0>;',
         '',
         '\t\tcompatible = "qcom,gpu-pwrlevels";']
    for i, lv in enumerate(table):
        s = nearest(stock, lv['freq'])
        note = 'stock %s' % name_of(s['level'])[len('RPMH_REGULATOR_LEVEL_'):] if s['freq'] == lv['freq'] else 'not in stock'
        if lv['raw'] != lv['level']:
            note += ', profile level %d' % lv['raw']
        L += ['',
              '\t\t/* %d MHz (%s) */' % (lv['freq'] // 1000000, note),
              '\t\tqcom,gpu-pwrlevel@%d {' % i,
              '\t\t\treg = <%d>;' % i,
              '\t\t\tqcom,gpu-freq = <%d>;' % lv['freq'],
              '\t\t\tqcom,level = <%s>;' % name_of(lv['level']),
              '',
              '\t\t\tqcom,bus-freq = <%d>;' % lv['bus-freq'],
              '\t\t\tqcom,bus-min = <%d>;' % lv['bus-min'],
              '\t\t\tqcom,bus-max = <%d>;' % lv['bus-max']]
        if lv['acd'] is not None or lv['cx-level'] is not None:
            L.append('')
        if lv['acd'] is not None:
            L.append('\t\t\tqcom,acd-level = <0x%x>;' % lv['acd'])
        if lv['cx-level'] is not None:
            L.append('\t\t\tqcom,cx-level = <%s>;' % name_of(lv['cx-level']))
        L.append('\t\t};')
    L += ['\t};', '};', '']
    open('%s/%s-gpu-%s.dts' % (outdir, soc, kind), 'w').write('\n'.join(L))
    print(soc, kind, desc, '%d levels, max %d MHz, initial %d' % (len(table), table[0]['freq'] // 1000000, init))
