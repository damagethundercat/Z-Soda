# Model Backbone Reassessment for Z-Soda

Date: 2026-03-07

## Why this document exists

Z-Soda has spent substantial effort trying to raise quality and stability on top of
`Depth Anything v3`, especially with tiled refinement, quality boost, multiview,
and runtime acceleration experiments. Recent review suggests that one of the main
problems may be upstream of all of those efforts: the currently selected backbone
family may not match the actual product goal.

The product goal is not "general visual geometry from arbitrary inputs."
The product goal is:

- one RGB image in
- one compositing-friendly depth map out
- strong monocular separation quality
- good edge behavior for AE workflows
- optional temporal stabilization as a later layer

That goal is much closer to "single-image monocular depth estimation for matte
generation" than to "general geometry understanding."

## What the official materials suggest

### Depth Anything V2

Official repo:
- https://github.com/DepthAnything/Depth-Anything-V2

Project page:
- https://depth-anything-v2.github.io/

Interpretation:
- DA2 is positioned as a monocular depth estimation model family.
- Its framing is aligned with image-to-depth use cases.
- This is much closer to Z-Soda's current product need.

### Depth Anything 3

Official repo:
- https://github.com/ByteDance-Seed/Depth-Anything-3

Project page:
- https://depth-anything-3.github.io/

Interpretation:
- DA3 is framed around spatially consistent geometry from arbitrary visual inputs.
- The emphasis is broader than monocular depth only: multi-view, pose, 3D
  consistency, geometry reasoning, and reconstruction-oriented behavior.
- The official project itself separates `DA3 Main Series` from `DA3 Monocular Series`.
- That separation is an important clue: the general DA3 family is not a single
  "better DA2" axis for all use cases.

### Distill Any Depth

Official repo:
- https://github.com/Westlake-AGI-Lab/Distill-Any-Depth

Project page:
- https://distill-any-depth-official.github.io/

Interpretation:
- Distill Any Depth is explicitly framed as a stronger monocular depth estimator.
- Official benchmarks and qualitative examples are presented against DA2-family
  baselines.
- This makes QD3's apparent use of a DistillAnyDepth-family approach look
  reasonable rather than surprising.

## Practical reading for Z-Soda

The likely mistake was assuming:

- newer model generation
- broader geometry ability
- better benchmark numbers

would automatically produce better AE depth maps.

That assumption is weak for this product.

For Z-Soda, the winning model is not necessarily the most general geometry model.
It is the model that best produces:

- clean front/back ordering
- useful face/object relief
- stable region separation
- compositing-friendly gradients
- strong thin-structure behavior

Those are product-output criteria, not general visual geometry criteria.

## Working conclusion

The current issues are likely caused by two different layers at once:

1. Runtime instability and fallback behavior
   - current logs still show ORT fallback episodes and loader conflicts
   - this makes visual comparison noisy and can hide or flatten quality differences

2. Backbone-goal mismatch
   - DA3 Main Series may simply be the wrong default family for the primary
     Z-Soda use case
   - even a perfectly integrated DA3 Main path may still underperform a
     monocular-depth-specialized backbone for AE output quality

The second point is strategic and more important than another round of local
boost tuning.

## Recommended direction reset

### Product default

Treat the main Z-Soda target as:

- single-frame monocular depth quality first
- temporal consistency second
- multiview and geometry-heavy paths as optional or experimental

### Backbone candidates to prioritize

Primary evaluation set:

1. Depth Anything V2 Large
2. Distill Any Depth Large
3. Depth Anything 3 Monocular Series

Keep as experimental / non-default:

4. Depth Anything 3 Main Series
5. current multiview-heavy paths

### Immediate engineering rule

Do not keep tuning `quality boost`, tiling, or temporal logic as if the current
backbone choice is already settled.

That risks spending time polishing the wrong base model.

## Recommended experiment order

1. Fix runtime path enough that comparisons are not masked by fallback output.
2. Define a small still-image comparison corpus for AE-relevant cases:
   - faces
   - thin structures
   - layered interiors
   - illustrations / stylized art
   - foliage / hair / fingers
3. Compare raw single-frame outputs first.
4. Only after that, compare postprocessed outputs.
5. Only after that, add temporal stabilization as a separate layer.

## Decision proposal

Short version:

- demote `DA3 Main` from default backbone
- stop assuming `v3 > v2` for this product
- treat `DistillAnyDepth` as a serious primary candidate
- evaluate `DA2`, `DistillAnyDepth`, and `DA3 Monocular` on a fixed AE-oriented
  corpus before further pipeline complexity is added

## Notes for the next agent

This is not a request to delete the current DA3 path immediately.

It is a request to change the evaluation frame:

- separate "model family choice" from "pipeline tuning"
- separate "single-frame quality" from "temporal consistency"
- separate "general geometry" from "compositing depth quality"

The next useful milestone is not another boost tweak.
It is a backbone comparison plan with a clear default-candidate decision.

## Current caveat

As of this note, there is also an unresolved ONNX Runtime loader/fallback issue in
the worktree and AE logs. That issue should be kept logically separate from the
backbone decision. A model strategy decision should not be blocked on finishing
the current DA3 runtime experiments.
