# DoraCore Kernel Tuner

Magisk module for DoraCore SM8450/SM8475 kernels.

## Profile

- WALT governor preferred when available
- uclamp min `224`, max `1024`
- aggressive WALT migration/input boost defaults
- zram forced to `zstd`
- zram size: 65% RAM on SM8450, 75% RAM on SM8475
- VM defaults aligned with kernel: swappiness `120`, page-cluster `0`, watermark scale `125`, vfs cache pressure `70`

## Boot behavior

`service.sh` waits for boot completion, sleeps 20 seconds, then reapplies tuning 12 times to outlast vendor post-boot overrides.

## Notes

This profile is performance-first. Expect higher heat and power draw.
