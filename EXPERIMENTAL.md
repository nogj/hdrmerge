# HDRMerge 0.6.0 experimental

This branch is an experimental functional evolution of `release-v0.6`. It is
intended for testing with copies of source files, not as a replacement for a
validated stable release.

## Main improvements

- Safer RAW loading, meaningful CLI exit codes and visible save errors.
- Output collision protection and temporary-file based DNG replacement.
- Zero black level in floating-point output to preserve shadow precision.
- Conservative channel-safe clipping threshold.
- Common-reference integer, subpixel and affine alignment without accumulated chain drift.
- CFA-safe Bayer resampling with confidence checks and automatic integer fallback.
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
--alignment integer|subpixel|affine
```

`subpixel` is the default alignment mode. `affine` additionally corrects small
rotation and scale changes, but rejects transformations outside conservative
limits. Both refined modes preserve the four Bayer phases independently and
exclude invalid warped borders from merging. Use `-v` to inspect confidence
and fallback messages.

Always retain backups of source RAW files. The program prevents direct output
collisions, but this branch still needs validation across a broad camera set.

## Known limitations

- CFA-safe subpixel and affine resampling currently supports repeating 2x2
  Bayer patterns. X-Trans and uncommon CFA layouts fall back to integer mode.
- Affine alignment is deliberately limited to small rotations and scale
  changes and still requires validation across a broader camera set.
- Linear DNG input is not yet supported as a separate RGB processing path.
- The DNG writer still renders the complete output in memory; very large or
  numerous files can therefore require substantial RAM.
- Deghosting and exposure averaging are intentionally opt-in and should be
  checked visually for every camera and scene type.
