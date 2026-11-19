<!-- SPDX-License-Identifier: GPL-2.0-only -->

This platform builds Xiaomi SM8450, SM8475 and SM7475 with kernel 6.6.
Supported build targets are cupid, diting, marble, mayfly, mondrian, thor,
unicorn, zeus and zizhan. Ziyi/SM7450 is not enabled.

On an existing LineageOS Xiaomi SM8450 checkout, install
`sm8450-manifest.xml` as `.repo/local_manifests/xiaomi_sm8450_6.6.xml`.
It selects the kernel, device, vendor, display and audio branches required
by this platform. Remove duplicate project declarations from other local
manifests. MIUI Camera and other local product additions are optional and
are not selected by this manifest.

Configure and build from the Android checkout root:

```sh
source build/envsetup.sh
breakfast marble
m bacon
```

Substitute any supported device for marble. The audio `target_product`
Bazel flag in `BoardConfigCommon.mk` must match the selected device.

Compile and check all supported base/overlay combinations:

```sh
python3 kernel/platform/kernel-6.6/vendor/xiaomi/sm8450-devicetrees/tools/validate-waipio.py --out out/sm8450-dt-validation
```

The validator checks bindings consumed by the drivers and supports optional
firmware and working-device baselines. It does not replace hardware testing
or full dt-schema validation.
