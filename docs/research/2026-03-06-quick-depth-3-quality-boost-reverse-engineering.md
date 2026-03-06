# 2026-03-06 Quick Depth 3 Quality Boost Reverse Engineering

## Summary

Quick Depth 3 `Quality Boost` is not a post sharpen slider.

The traced `render` path shows that:

- `boost` is read into `n_tiles`
- `quality` is temporarily clamped to `min(2, user_quality)` for a full-frame reference pass
- tiled local re-inference then runs at the original user quality
- each tile is aligned back to the corresponding reference region with local mean/std matching
- overlapping tiles are merged with a feather mask

This explains why enabling `Quality Boost` changes low-frequency gradients across the frame in addition to local detail.

## Evidence

Runtime disassembly captured from the interactive AE run is stored in:

- `artifacts/qd3_runtime_trace/trace_disassembly.txt`
- `artifacts/qd3_runtime_trace/trace_log.txt`
- `C:/Users/Yongkyu/AppData/Local/Temp/BSKL/quickdepth3_plugin.log`

Key findings from the captured `render` function:

- `params.get("boost", 0)` is stored in local `n_tiles`
- `if n_tiles > 1` enters the tiled refinement path
- `params["quality"] = min(2, user_quality)` is applied before `ref_depth = render_dad(...)`
- `processed_tile = render_dad(tile, ...)` runs for each padded tile
- `processed_np` is normalized to the local `ref_region` using:
  - `ref_mean`, `ref_std`
  - `tile_mean`, `tile_std`
  - `(processed_np - tile_mean) * (ref_std / tile_std) + ref_mean`

Key findings from the captured `render_dad` function:

- `keep_aspect_ratio` is read directly from `plugin.params`
- `Resize(... keep_aspect_ratio=params.get("keep_aspect_ratio", False), ensure_multiple_of=14, resize_method="lower_bound")`
- prediction is min-max normalized per call before conversion back to image space
- optional time consistency also uses mean/std alignment against the stored previous frame

## Effective Behavior

For a user setting of `quality=768px` and `boost=4`:

- user quality maps to internal `quality=3`
- boost value maps to `n_tiles=4`
- the reference map is generated at `quality=2`, which maps to `512px`
- the local refinement pass uses a `4 x 4` tile grid with `50%` padding

So the effective structure is:

1. Full-frame reference depth at reduced quality
2. High-quality per-tile re-inference
3. Per-tile local stat alignment to the reference map
4. Feathered composition into a refined output

## Why the Frame Gradient Matters

The top/bottom gradient observed in QD3 output is a strong clue because it cannot be explained by edge-only sharpening.

That gradient is consistent with:

- a global reference pass that establishes low-frequency depth layout
- local tile detail being constrained to that reference through stat matching

In short, `Quality Boost` changes both:

- global depth distribution
- local structural detail

## Implications for Z-Soda

The closest practical prototype in Z-Soda is:

1. Add a separate `Quality Boost` parameter instead of overloading `quality`
2. Run a lower-quality full-frame reference pass when boost is enabled
3. Run tiled local inference at the selected quality
4. Align each tile to the corresponding reference region before merge
5. Apply postprocess only after the boosted raw depth has been composed

The current Z-Soda detail boost only performs a whole-image percentile alignment after tiled inference. That is materially different from QD3 and does not reproduce the same frame-wide gradient behavior.
