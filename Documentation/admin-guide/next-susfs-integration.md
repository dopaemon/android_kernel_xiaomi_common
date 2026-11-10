# KernelSU-Next / SUSFS integration

This tree follows the Android 15 / 6.6 recipe in
`WildKernels/GKI_KernelSU_SUSFS` at
`c6d44eb9110888ad0cbc8e172d0d64eaa7df3e1e`, applied to the Xiaomi kernel
base `6c93f14f14f5` (6.6.144).

## Sources

| Component | Repository | Revision |
| --- | --- | --- |
| KernelSU-Next | https://github.com/pershoot/KernelSU-Next (`dev-susfs`) | `e152fbbaa4aaa67517663e162039d209a7aba08e` |
| SUSFS | https://gitlab.com/simonpunk/susfs4ksu (`gki-android15-6.6`) | `ec03d0c73b821afa1d5037d6d493216f59744fcb` |
| Patch collection | https://github.com/WildKernels/kernel_patches | `b6690c6c66a05f873faf460daaaac1c84de9d720` |
| Baseband Guard | https://github.com/vc-teahouse/Baseband-guard | `028ff7fd509e2104596da7cc1ef8e6ce2a0c9082` |
| NoMount | https://github.com/maxsteeel/nomount (`dev`) | `c7d3fff08a07bde1d5b0f419e950d4aef71dce89` |
| DroidSpaces | https://github.com/ravindu644/Droidspaces-OSS | `cdadb9eb5d0f86aacffff60776333e6f6b73a30a` |

KernelSU-Next, Baseband Guard and NoMount are imported with full Git subtree
history, without squashing. They are tracked source files, so a normal clone
contains the complete integration; no external checkout is required to build.
Mailbox patches retain their upstream author, author date and message. Raw
patches record their source revision in the corresponding commit message.

## Application order

1. Import KernelSU-Next from `pershoot/dev-susfs`. This is the kernel-side
   implementation selected by the recipe when Next and SUSFS are enabled.
2. Apply `50_add_susfs_in_gki-android15-6.6.patch` and copy the SUSFS sources
   and headers from the same revision. Next uses the current SUSFS branch,
   rather than the verified pin for other root flavors.
3. Apply pershoot's SUSFS commits, in recipe order:
   `6387756d64b3ab9ececefc4523627a9ee05a7519` and
   `161c058fed5544a6573d87124f346dadd5c4962d`.
4. Wire `drivers/kernelsu`, apply the priority helper and fanotify compatibility
   scripts, and pin Next's version to the upstream `dev` merge-base
   `b90876426197f5d35c3003bf42d8bf927dc20181` (version 33501, tag v3.4.2).
5. Apply the Android 15 / 6.6 BBRv3 patch and integrate Baseband Guard.
6. Apply DroidSpaces' `fix_sysvipc_kabi_6_7_8` patch, then the 6.6 NTSync
   compatibility patch, NTSync base patch and Unicode 6.1+ fix.
7. Integrate NoMount and enable the recipe's default feature configuration
   in `arch/arm64/configs/gki_defconfig`. Add the Xiaomi
   `device_find_any_child` ABI symbol from the device action.

## Adaptations to this tree

- BBRv3: retain the existing `tcp_in_ack_event()` call and pass `&rs` to
  `tcp_process_tlp_ack()` at its current location. Omit the upstream patch's
  unrelated comment typo (`from:->`).
- DroidSpaces: retain the existing runtime-verification block following the
  task ABI reserves; the patch's old trailing comment context is obsolete.
  The SYSVIPC fields still occupy ABI reserves 6/7/8 as upstream intended.
- Component versions are pinned in their build files because subtree imports
  share the kernel Git repository. This prevents reporting the enclosing
  kernel's commit count or SHA as a component version.
- The kernel Makefile name, localversion, `scripts/setlocalversion`, Xiaomi
  build configuration and `wonder.fragment` are preserved. The branding and
  dirty-flag rewrite actions are intentionally omitted.
- The Samsung min_kdp shim is not added to this Xiaomi device tree.
- Ptrace is gated to older kernels by the recipe. The old-sublevel SUSFS
  context fixes are unnecessary on 6.6.144. Performance patches are disabled
  by default upstream and are not enabled here.

The official KernelSU-Next manager should match the upstream `dev` base noted
above; the recipe uses pershoot's fork for the kernel SUSFS integration.

## Validation

Use Clang/LLVM and an out-of-tree output directory:

```sh
make O=../out ARCH=arm64 LLVM=1 gki_defconfig
make O=../out ARCH=arm64 LLVM=1 CONFIG_UAPI_HEADER_TEST=n -j32 vmlinux
```

Validation completed with Clang 18: GKI defconfig generation, compilation of
all modified subsystems, MODPOST, BTF generation and final vmlinux linking.
All imported components remain ancestors of HEAD, and their integration
symlinks resolve within the tracked tree.

The unmodified default build fails the existing Xiaomi UAPI header test in
`include/uapi/media/ispv3_defs.h` (incomplete `struct timespec` and unknown
`uint32_t`/`uint64_t`/`int32_t` types). That header and `videodev2.h` are
unchanged from the kernel base. The final link check above disables only
that test via a make command-line override; the committed configuration
retains `CONFIG_UAPI_HEADER_TEST=y`. Existing duplicate device-tree target
warnings and BTF duplicate-type-ID warnings remain in the build logs.

Build logs are stored outside the kernel repository in
`../sources/build-check.log`, `../sources/vmlinux-check.log` (default header
test failure), and `../sources/vmlinux-link-check.log` (successful link).
This checks the GKI configuration. A device build must also use the Xiaomi
build configuration and matching vendor modules before flashing; boot and
on-device behavior have not been tested.
