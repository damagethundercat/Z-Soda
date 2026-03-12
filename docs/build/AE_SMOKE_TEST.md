# AE Smoke Test Checklist

## Purpose

Verify the current shipping `Z-Soda` AE effect at a practical smoke-test level:
load, main UI controls, interactive updates, preview stability, and render queue output.

## Preconditions

- `ZSoda.aex` is installed in MediaCore
- A test comp is ready
  - 1920x1080
  - 30 fps
  - at least one footage layer with visible depth separation
- If verbose host/router tracing is needed, launch AE with `ZSODA_AE_TRACE=1`

## Record Before Testing

- AE version / OS / GPU
- `ZSoda.aex` build timestamp or commit
- Test project path
- Any runtime log or screenshot captured during failure

## Test Cases

### ST-01 Plugin Load

Steps:
1. Launch AE and open the test project.
2. Apply `Z-Soda` to a footage layer.

Pass:
- The effect appears in the effect list.
- The effect applies without AE crashing.
- The Effect Controls panel opens normally.

Fail:
- The effect is missing.
- AE crashes or rejects the effect during apply.

### ST-02 Shipping Controls Visible

Expected controls:
- `Quality`
- `Preserve Ratio`
- `Output`
- `Slice Mode`
- `Position (%)`
- `Range (%)`
- `Soft Border (%)`

Pass:
- The controls above are visible.
- Old controls such as model selector, `Normalize`, `Absolute Depth`, `Minimum`, and `Maximum` are not visible.

Fail:
- Missing shipping controls.
- Old removed controls reappear.

### ST-03 Quality Update

Steps:
1. Set `Output` to `Depth Map`.
2. Change `Quality` across multiple values.

Pass:
- The render updates without crashing.
- Visible quality/resolution changes are reflected in the result.

Fail:
- No visible change across quality values.
- AE freezes or crashes while changing `Quality`.

### ST-04 Depth Map Output

Steps:
1. Keep `Output` on `Depth Map`.
2. Move across multiple frames in the comp.

Pass:
- Output behaves like a depth visualization rather than a flat constant image.
- Frame changes update the result normally.

Fail:
- Output stays flat or invalid.
- Frame updates stop or produce unstable flashing.

### ST-05 Depth Slice Interaction

Steps:
1. Set `Output` to `Depth Slice`.
2. Change `Slice Mode` between `Near`, `Far`, and `Band`.
3. Adjust `Position (%)`, `Range (%)`, and `Soft Border (%)`.
4. Click each slider arrow at least once.

Pass:
- Matte/output changes immediately as the controls change.
- Slider arrow clicks do not crash AE.
- `Near`, `Far`, and `Band` produce distinct behaviors.

Fail:
- Slice controls do not affect the result.
- Arrow-button clicks freeze or crash AE.

### ST-06 Preview and Render Queue

Steps:
1. Run RAM Preview on a short range.
2. Add the comp to Render Queue and render a short output.

Pass:
- Preview completes and plays back.
- Render Queue completes without plugin crash.
- The rendered result matches the selected output mode.

Fail:
- AE hangs or exits during preview/render.
- Output file does not match the interactive result.

## Result

- `SMOKE PASS`: all cases pass
- `SMOKE FAIL`: any case fails

## Failure Template

- Case ID:
- Time:
- Environment:
- Repro steps:
- Expected:
- Actual:
- Attachments:
