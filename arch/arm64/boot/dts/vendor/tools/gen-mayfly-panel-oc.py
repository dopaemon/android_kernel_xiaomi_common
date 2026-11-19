#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0
#
# Generate the mayfly display overclock timings (mayfly-panel-oc-{144,156}hz.dtsi)
# from the 120 Hz timing@1 of the panel and board dtsi.
#
# usage (from arch/arm64/boot/dts/vendor/qcom/display/display):
#   ../../../tools/gen-mayfly-panel-oc.py dsi-panel-l3s-42-02-0a-dsc-cmd.dtsi \
#       mayfly-sde-display-mtp.dtsi .
import re, sys
panel, board, outdir = sys.argv[1:4]
src = open(panel).read().split('\n')

def blocks(lines, header_re):
    """Return bodies (list of lines) of every node matching header_re."""
    out = []
    i = 0
    while i < len(lines):
        if re.search(header_re, lines[i]):
            depth = 0; body = []
            for j in range(i, len(lines)):
                depth += lines[j].count('{') - lines[j].count('}')
                if j > i:
                    body.append(lines[j])
                if depth == 0:
                    break
            out.append(body[:-1]); i = j
        i += 1
    return out

t1 = blocks(src, r'^\s*timing@1\s*\{')
if len(t1) != 2:
    sys.exit('expected 2 timing@1 nodes in panel dtsi, got %d' % len(t1))
qcom_part, mi_part = t1
bt = blocks(open(board).read().split('\n'), r'^\s*timing@1\s*\{')
if len(bt) != 1:
    sys.exit('expected 1 timing@1 node in board dtsi')
board_part = bt[0]

def body(lines):
    tabs = [len(l) - len(l.lstrip('\t')) for l in lines if l.strip()]
    cut = min(tabs) if tabs else 0
    return ['\t\t\t' + l[cut:] if l.strip() else '' for l in lines]

for hz, clk, node in ((144, 998400000, 3), (156, 1331200000, 4)):
    q = '\n'.join(qcom_part)
    q, n1 = re.subn(r'qcom,mdss-dsi-panel-framerate = <120>;', 'qcom,mdss-dsi-panel-framerate = <%d>;' % hz, q)
    q, n2 = re.subn(r'qcom,mdss-dsi-panel-clockrate = <832000000>;', 'qcom,mdss-dsi-panel-clockrate = <%d>;' % clk, q)
    if n1 != 1 or n2 != 1:
        sys.exit('120 Hz timing does not have the expected framerate/clockrate')
    lines = [
        '/*',
        ' * DoraCore: %d Hz display overclock for the mayfly',
        ' * "xiaomi 42 02 0a mp cmd mode dsc dsi panel".',
        ' *',
        ' * Copy of the 120 Hz timing@1 with only the frame rate and the DSI',
        ' * panel clock raised, as in the overclocked stock dtbo (%d Hz at',
        ' * %d Hz DSI clock). The panel is calibrated for 120 Hz, so colours',
        ' * may shift at this rate. Generated from',
        ' * dsi-panel-l3s-42-02-0a-dsc-cmd.dtsi and mayfly-sde-display-mtp.dtsi by',
        ' * tools/gen-mayfly-panel-oc.py; regenerate when the 120 Hz timing changes.',
        ' */',
        '',
        '&dsi_l3s_42_02_0a_dsc_cmd {',
        '\tqcom,mdss-dsi-display-timings {',
        '\t\t/* %d Hz (overclock) */',
        '\t\ttiming@%d {',
    ]
    lines = [l % (hz,) if l.count('%d') == 1 and 'timing@' not in l else l for l in lines]
    lines[5] = ' * panel clock raised, as in the overclocked stock dtbo (%d Hz at' % hz
    lines[6] = ' * %d Hz DSI clock). The panel is calibrated for 120 Hz, so colours' % clk
    lines[-2] = '\t\t/* %d Hz (overclock) */' % hz
    lines[-1] = '\t\ttiming@%d {' % node
    lines += body(q.split('\n'))
    lines += ['', '\t\t\t/* Xiaomi panel commands, as at 120 Hz */']
    lines += body(mi_part)
    lines += ['', '\t\t\t/* mayfly DSI PHY and topology, as at 120 Hz */']
    lines += body(board_part)
    lines += ['\t\t};', '\t};', '};', '']
    out = re.sub(r'\n{3,}', '\n\n', '\n'.join(lines))
    open('%s/mayfly-panel-oc-%dhz.dtsi' % (outdir, hz), 'w').write(out)
    print('wrote', hz, len(out.split('\n')), 'lines')
