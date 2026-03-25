# Thin Release and First-Run Setup Design

Historical note: this thin bootstrap design is no longer the active shipping
direction. Keep it only as archived product exploration; do not treat it as the
current release contract.

This document records the planned shift from a self-contained shipping bundle
to a thin plug-in package with first-run setup.

## Problem

The current self-contained release contract solved zero-install bring-up, but
it also pushed the Windows package into the 400MB+ range because the shipped
artifact contains:

- the native plug-in
- a portable Python runtime
- `torch`, `transformers`, `Pillow`, and helper dependencies
- a local `distill-any-depth-base` model snapshot

That size is not a good default fit for the current target audience.

## Product Decision

The default shipping path will move to:

1. user downloads a thin `ZSoda.aex` or `ZSoda.plugin`
2. user copies that plug-in file into the AE plug-in folder
3. user launches After Effects
4. user applies `Z-Soda`
5. the plug-in performs a one-time setup into a per-user cache
6. depth inference starts automatically once setup is ready

The design intentionally drops the requirement that release packages work fully
offline out of the box.

## UX Contract

### What the user sees

- The public AE control surface stays the same 8-control UI.
- On the first real use, the effect should not fall back to dummy depth.
- Instead, the effect should render a visible setup slate while background setup
  progresses.
- Once the required assets are ready, the effect should switch to real depth
  output without requiring the user to reinstall the plug-in.

### What the user should not see

- No hidden model download during AE application startup.
- No silent dummy-engine output that looks like a successful depth render.
- No visible model picker, advanced runtime controls, or setup-only controls in
  the shipping UI.

## Why first use, not AE startup

Downloading at AE startup is a poor default because it:

- adds network work even if the user never applies `Z-Soda`
- makes AE launch behavior feel slower and harder to explain
- couples plug-in discovery with runtime provisioning
- hides the setup moment from the user

The preferred trigger is the first real render path for a `Z-Soda` effect
instance. A later optimization may move the kickoff earlier to sequence setup,
but only after the setup UX is already explicit and visible.

## Distribution Shape

### Thin release artifact

The release zip should expose only the native plug-in file plus optional
checksum metadata:

- `ZSoda.aex`
- `ZSoda.aex.sha256`

The native plug-in is responsible for pulling the runtime assets it needs.

### Remote assets

The native plug-in will fetch a small bootstrap manifest first, then download
versioned asset bundles from release hosting.

Required remote assets:

- `bootstrap-manifest.json`
- `zsoda-runtime-win-x64-<version>.zip`
- `distill-any-depth-base-hf-<revision>.zip`

Optional future split:

- helper/runtime bundle
- model bundle
- shared HF cache bundle

The initial implementation should favor the simplest reliable bundle layout over
maximum deduplication.

## Local Cache Layout

Windows target layout:

```text
%LOCALAPPDATA%/ZSoda/
  bootstrap/
    state.json
    bootstrap-manifest.json
    downloads/
      runtime.zip.partial
      model.zip.partial
    locks/
      setup.lock
    runtime/
      <runtime_id>/
        zsoda_py/
        python/
    models/
      <model_revision>/
        models/
          hf/
            distill-any-depth-base/
              ...
```

Rules:

- all downloads land in `downloads/` first
- checksum verification happens before promotion into `runtime/` or `models/`
- extraction must be atomic from the point of view of the render path
- versioned directories stay reusable across plug-in updates when compatible

## Bootstrap State Machine

The plug-in should track an explicit setup state instead of mapping setup
problems onto the dummy engine path.

Proposed states:

- `kNotStarted`
- `kFetchingManifest`
- `kDownloadingRuntime`
- `kDownloadingModel`
- `kVerifying`
- `kExtracting`
- `kReady`
- `kFailed`

Each state should carry:

- short user-facing status text
- progress value from `0.0` to `1.0` when available
- last error string when failed
- resolved runtime/model ids once ready

## Runtime Integration

### New native bootstrap manager

Add a native bootstrap component under `plugin/inference/` that owns:

- manifest download and parse
- bundle download
- checksum validation
- extraction/promotion into cache
- persisted setup state
- cross-render reuse and single-flight locking

Working name:

- `RuntimeAssetBootstrap`
- `BootstrapManifest`
- `BootstrapStateStore`

### Engine behavior

`ManagedInferenceEngine` should be changed so that release builds:

- request setup readiness from the bootstrap manager
- do not use `DummyInferenceEngine` as the normal release fallback path
- report `setup pending` or `setup failed` explicitly
- only configure the remote backend after the bootstrap manager reports `kReady`

The existing file-level `ModelAutoDownloader` path is not a good primary fit for
the DistillAnyDepth remote-service release flow. It should become debug-only,
legacy-only, or be replaced by the bundle bootstrap path.

### Remote runtime resolution

Once setup is ready, the remote backend should prefer the downloaded cache root
for:

- `distill_any_depth_remote_service.py`
- bundled Python executable
- bundled local HF model repo
- optional bundled HF cache

This fits the current runtime search-root design and should reuse the existing
`runtime_asset_root` and remote-service path resolution flow where possible.

## Render and AE UX Integration

### Render pipeline states

`RenderPipeline` should gain explicit non-success statuses for setup:

- `kSetupPending`
- `kSetupFailed`

These are distinct from:

- `kSafeOutput`
- `kFallbackTiled`
- `kFallbackDownscaled`

### Visible setup slate

When setup is pending or failed, the pipeline should generate a visible output
frame instead of pretending inference succeeded.

The first implementation should render a simple status card with:

- `Z-Soda setup`
- a one-line status message
- percent progress when known
- a short retry/failure hint when setup failed

This keeps the experience understandable even if AE exposes no dedicated setup
dialog surface.

### Status messaging

The existing `RenderOutput.message` field should continue carrying a detailed
string for diagnostics. The setup slate should use a shorter, user-facing form
derived from the same state.

## Network and Integrity Rules

- Manifest and bundle downloads must use HTTPS.
- Each remote asset must have a declared SHA256 in the bootstrap manifest.
- Partial files must not be treated as ready assets.
- Failed verification must delete the bad partial and leave the previous ready
  cache intact.
- The bootstrap manager should never block the AE UI thread longer than needed
  to enqueue or poll background work.

## Proposed Bootstrap Manifest Shape

Illustrative JSON:

```json
{
  "schema": "zsoda.bootstrap.v1",
  "plugin_min_version": "0.1.0",
  "platform": "windows-x64",
  "runtime": {
    "id": "python312-torch210-win64-r1",
    "url": "https://example.invalid/zsoda-runtime-win-x64-r1.zip",
    "sha256": "..."
  },
  "model": {
    "id": "distill-any-depth-base",
    "revision": "hf-snapshot-r1",
    "url": "https://example.invalid/distill-any-depth-base-r1.zip",
    "sha256": "..."
  }
}
```

The native plug-in should also support environment overrides for local testing,
for example:

- `ZSODA_BOOTSTRAP_MANIFEST_URL`
- `ZSODA_BOOTSTRAP_ROOT`
- `ZSODA_BOOTSTRAP_DISABLE_NETWORK`

## Scope Boundaries

### In scope

- thin default release packaging
- first-use runtime/model bootstrap
- per-user cache
- visible setup progress
- explicit setup failure state
- removal of dummy-depth-as-success behavior for release setup cases

### Out of scope for the first pass

- offline full package parity
- multiple production models in the shipping UI
- authenticated/private asset hosting
- background updates while AE is idle
- macOS bootstrap implementation

## Implementation Phases

### Phase 1: Bootstrap foundation

- add bootstrap manifest format
- add native download/extract/checksum manager
- add persisted setup state and lock handling
- wire downloaded cache root into runtime path resolution

### Phase 2: Render UX

- add `kSetupPending` and `kSetupFailed`
- add setup slate rendering
- stop using dummy-depth output as the primary release path during setup

### Phase 3: Release packaging pivot

- stop embedding full runtime/model payloads by default
- emit thin plug-in release artifacts
- document hosting/publishing of runtime/model bundles
- update smoke tests for first-run setup behavior

## Validation

### Automated

- manifest parsing
- checksum mismatch handling
- interrupted download recovery
- state-store round trips
- bootstrap single-flight behavior
- runtime path resolution from downloaded cache root

### Manual

1. Install thin `ZSoda.aex`
2. Launch AE and apply `Z-Soda`
3. Confirm setup slate appears immediately
4. Confirm progress advances while assets download
5. Confirm the first successful render uses the real remote DAD path
6. Relaunch AE and confirm no setup slate appears when cache is already ready

## Migration Notes

The self-contained release helpers should remain available only until thin
bootstrap reaches parity. After thin bootstrap is validated, the docs and
release lane should treat self-contained packaging as legacy or internal-only.
