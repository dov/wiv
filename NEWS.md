# NEWS

## Changes since v0.1.0 (version 0.1.1)

### Thumbnails
- Thumbnail decoding is now prioritized by what is on screen: the thumbnails exposed by the
  strip's horizontal scrollbar are decoded first (nearest the middle of the view), followed by a
  half-viewport margin on each side. Decodes queued for thumbnails that have scrolled out of
  range are dropped, and running ones that haven't started loading are cancelled.
- Decoding runs on a dedicated pool of up to 8 worker threads.
- Faster, better downsampling: thumbnails are reduced with an exact area average (alpha-aware),
  about 3x faster than before. It uses Google Highway, which picks the best instruction set at
  run time on each machine (SSE2/SSE4/AVX2/AVX-512 on x86, NEON on ARM). The strip's squares
  are scaled the same way, which removes aliasing.
- Faster windowing of FITS / 16-bit images when generating thumbnails.
- Thumbnails are regenerated once on first visit to a folder (cache format v8).
- New thumbnail refresh button; the strip's thumbnails are centered when they don't fill its width.

### Viewer
- Histogram: log y-zoom, cursor marker, percentile auto contrast and gamma.
- The remembered view and the Auto Fit setting are kept across restarts.
- Left-drag pans by default.
- Fix cropped export; wiv asks what to save when an image is displayed.
- SVG mark sprites are hoverable and show the owner's balloon.
- Transparent images and SVGs are shown on a checkerboard that follows the theme's brightness,
  instead of a fixed white canvas. View > Transparency Background chooses between the
  checkerboard, the theme color, the Preferences color, white and black (Shift+B cycles them).
  `.giv` files keep giv's convention (Preferences color, white by default, or `$background`).

### Formats
- New netCDF image plugin.

### Build
- New dependency: Google Highway (`highway` in vcpkg; fetched automatically if not installed).
