# HDRMerge 0.6.0 experimental

This branch is an experimental functional evolution of `release-v0.6`. It is
intended for testing with copies of source files, not as a replacement for a
validated stable release.

## Main improvements

- Safer RAW loading, meaningful CLI exit codes and visible save errors.
- Output collision protection and temporary-file based DNG replacement.
- Zero black level in floating-point output to preserve shadow precision.
- Conservative channel-safe clipping threshold.
- Common-reference alignment to avoid accumulated chain drift.
- Optional automatic motion deghosting and multi-exposure noise averaging.
- Optional exposure preservation across panorama bracket sets.
- CR3 and additional RAW extensions plus an all-files selector.
- Configurable batch group size and `%cf` common-filename token.
- Zoom, fit-to-window, neutral preview, clipping warnings and mask sidecars.
- Lightweight project files containing source paths and a PNG mask.

## Experimental controls

Automatic deghosting compares normalized pixels with the least-exposed
reference. Lower thresholds detect more motion but can replace clean pixels
with noisier ones. Noise averaging should only be used for static scenes.

CLI additions:

```
--gui
--nogui
--deghost N
--average
--preserve-exposure
--bracket-size N
```

Always retain backups of source RAW files. The program prevents direct output
collisions, but this branch still needs validation across a broad camera set.
