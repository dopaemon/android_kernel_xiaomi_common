#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0
#
# Generate display overclock timings (<device>-panel-oc-{144,156}hz.dtsi)
# for a "xiaomi 42 02 0a" command mode DSC panel driven at 832 MHz from the
# 120 Hz timing@1 of its panel and board dtsi.
#
# usage (from arch/arm64/boot/dts/vendor/qcom/display/display):
#   ../../../tools/gen-panel-oc.py mayfly dsi-panel-l3s-42-02-0a-dsc-cmd.dtsi \
#       mayfly-sde-display-mtp.dtsi .
#   ../../../tools/gen-panel-oc.py cupid dsi-panel-l3-42-02-0a-dsc-cmd.dtsi \
#       cupid-sde-display-mtp.dtsi .
import re, sys
device, panel, board, outdir = sys.argv[1:5]
src = open(panel).read().split('\n')
text = '\n'.join(src)
label = re.search(r'^\s*(\w+): qcom,mdss_dsi_\w+ \{', text, re.M).group(1)
panel_name = re.search(r'qcom,mdss-dsi-panel-name = "([^"]+)"', text).group(1)
if not panel_name.startswith('xiaomi 42 02 0a'):
    sys.exit('%s: the clock/transfer settings were only measured on the 42 02 0a panel' % panel)

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

# name: overclock step (file name); clk: DSI panel clock; xfer: MDP transfer
# time; fps: the declared frame rate, which is the TE rate the panel was
# measured to run at with that clock and transfer time on mayfly (144 ->
# 145.6 Hz, 156 -> 155.0 Hz; cupid has the same panel at the same 832 MHz
# clock), so that SurfaceFlinger and the display HAL pace frames
# on the panel's real vsync instead of drifting by a frame every second.
for name, fps, clk, xfer, node in ((144, 146, 998400000, 6340, 3),
                                   (156, 155, 1331200000, 5810, 4)):
    hz = fps
    q = '\n'.join(qcom_part)
    q, n1 = re.subn(r'qcom,mdss-dsi-panel-framerate = <120>;', 'qcom,mdss-dsi-panel-framerate = <%d>;' % hz, q)
    q, n2 = re.subn(r'qcom,mdss-dsi-panel-clockrate = <832000000>;', 'qcom,mdss-dsi-panel-clockrate = <%d>;' % clk, q)
    if n1 != 1 or n2 != 1:
        sys.exit('120 Hz timing does not have the expected framerate/clockrate')
    # The 120 Hz transfer time (7000 us) is longer than an overclocked
    # frame, so the DSI driver drops it and derives one from the DSI clock
    # instead (3920 us at 156 Hz), which needs more than the 500 MHz MDP
    # clock: SDM then fails every composition and the screen stays black.
    # Each overclocked timing gets its own transfer time, about 600 us short
    # of the frame, to keep the MDP clock well below that limit.
    q, n3 = re.subn(r'qcom,mdss-mdp-transfer-time-us = <\d+>;', 'qcom,mdss-mdp-transfer-time-us = <%d>;' % xfer, q)
    if n3 != 1:
        sys.exit('120 Hz timing does not have qcom,mdss-mdp-transfer-time-us')
    lines = [
        '/*',
        ' * DoraCore: %d Hz display overclock for the %s' % (name, device),
        ' * "%s", declared as %d Hz:' % (panel_name, fps),
        ' * the TE rate this panel was measured to run at with these settings.',
        ' *',
        ' * Copy of the 120 Hz timing@1 with the frame rate, the DSI panel',
        ' * clock (%d Hz) and the MDP transfer time (%d us) changed, as the' % (clk, xfer),
        ' * 120 Hz transfer time does not fit in a %d Hz frame. The panel is' % hz,
        ' * calibrated for 120 Hz, so colours may shift at this rate.',
        ' * Generated from %s and' % panel,
        ' * %s by tools/gen-panel-oc.py;' % board,
        ' * regenerate when the 120 Hz timing changes.',
        ' */',
        '',
        '&%s {' % label,
        '\tqcom,mdss-dsi-display-timings {',
        '\t\t/* %d Hz (overclock) */' % hz,
        '\t\ttiming@%d {' % node,
    ]
    lines += body(q.split('\n'))
    lines += ['', '\t\t\t/* Xiaomi panel commands, as at 120 Hz */']
    lines += body(mi_part)
    lines += ['', '\t\t\t/* %s DSI PHY and topology, as at 120 Hz */' % device]
    lines += body(board_part)
    lines += ['\t\t};', '\t};', '};', '']
    out = re.sub(r'\n{3,}', '\n\n', '\n'.join(lines))
    open('%s/%s-panel-oc-%dhz.dtsi' % (outdir, device, name), 'w').write(out)
    print('wrote', name, fps, len(out.split('\n')), 'lines')
