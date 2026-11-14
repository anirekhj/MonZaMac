# Experimental MetalFX temporal upscaling

Choose **Video > Upscaler > MetalFX temporal** to reconstruct the scene from its color, depth, motion and earlier frames. Internal resolution controls how many pixels the game renders; the window determines the reconstructed output size. Unlike MetalFX spatial, this mode accumulates history and provides anti-aliasing.

Temporal upscaling is opt-in and uses the temporal API available from macOS 13 on compatible MetalFX GPUs; MonZaMac’s own minimum macOS requirement still applies. Existing bilinear/spatial choices and defaults remain unchanged. It also works at equal input/output resolution as temporal anti-aliasing. Inputs larger than the output or outside the GPU's supported scaling range fall back to spatial processing. Menus, loading and missing scene inputs also use the spatial path. Hardware capability and scale limits are checked at runtime.

## With frame generation

Temporal upscaling can run alone or together with **30 -> 60** or **60 -> 120** frame generation. Combined mode requires macOS 26 and a GPU supporting MetalFX frame interpolation, just as standalone frame generation does. It reconstructs real frames first and shares the low-resolution depth and motion with Apple's scaler-linked frame interpolator; it does not prepare a second motion/depth field. Previous reconstructed color is retained separately for interpolation.

In combined mode, generated frames use the reconstructed output resolution. The saved Native/Performance choice is retained but inactive until temporal upscaling is turned off. Existing FPS overrides and the Developer uncapped benchmark still disable frame generation. Source FPS, displayed FPS and game timing retain the ordinary frame-generation behavior: selecting a target does not guarantee that the GPU reaches it.

The game FXAA and presenter SMAA preferences are preserved. Temporal reconstruction replaces them while active; switching to a spatial upscaler restores the saved choices. A developer `LARECOMP_FXAA` override still takes precedence. The persisted temporal selection is `upscaler = metalfx_temporal` in `larecomp.ini`.

## Image quality and limits

This is experimental. Skinned character motion is combined with camera/depth reconstruction for the rest of the scene; independently animated foliage, particles, water and transparency can still show trails or edge artifacts. A conservative difference mask keeps current-frame HUD pixels out of accumulated scene history; this is not a separate native-resolution HUD layer. Broad combat/effects testing remains necessary before recommending it for every scene.

Jitter is applied as a fractional Metal viewport offset to recognized G-buffer draws, with corresponding copies of the camera matrices for deferred reconstruction. Camera and geometry motion exclude that jitter. Guest memory and cached uniform snapshots are not modified. The translated shader text and shader-preparation version do not change, so this feature does not require regenerating all translated shaders.

Histories reset on menu/loading gaps, output-size/upscaler/frame-generation changes and camera cuts. Input eligibility uses the actual viewport, including handheld subrectangles. Reconstructed images are linear until presentation encodes sRGB. In-flight generations retain their resources through GPU completion.

## Native dynamic inputs and preparation caches

In Dynamic resolution modes, the temporal scaler keeps one maximum-size set of input allocations and changes `inputContentWidth` / `inputContentHeight` as the render size changes. Ordinary resolution steps preserve reconstruction history and the scaler linked to frame generation instead of rebuilding both effects. The allocation ceiling follows the selected Dynamic limit (1080p, 1620p or the output), preserving the input aspect ratio; some extra input memory is reserved compared with allocating only the current size. Output-size or input-aspect changes still create a new generation. Devices that reject dynamic initialization use the fixed-size path. `LARECOMP_TEMPORAL_DYNAMIC_INPUT=0` forces that path for comparison.

Size-independent preparation/composition pipelines are cached per Metal device across reconstruction resets and resizes. This removes repeated application-side shader compilation at those transitions; it is not a measured steady-state FPS improvement.

`LARECOMP_TEMPORAL_REACTIVE=1` enables an experimental MetalFX reactive mask. It compares the current linear scene with the previous scene reprojected by the available motion field, reducing history where a large photometric difference remains or previous coverage is absent. It does not identify materials, foliage or particles. Lighting changes and disocclusions can also trigger it; reducing history may introduce more aliasing. The mask is off by default pending gameplay visual checks, adds an R8 input and one compute dispatch, and falls back to ordinary reconstruction if mask initialization fails. Previous comparison inputs remain retained until GPU completion.

## Verification and measured screen

On Apple M5/macOS 26.5.1, native GPU tests cover matrix/coordinate invariants, both depth formats, input validation, scale/menu eligibility, resets, queued-resource lifetime and resize. Frame-generation tests cover both source rates and a known interpolation midpoint with low-resolution depth/motion and higher-resolution color associated with the temporal scaler. Existing renderer and settings tests cover the unchanged default paths and saved preferences. Additional M3 Max synthetic GPU checks exercise changing input sizes without history/scaler resets, known camera motion in active-input pixels, reconstructed and generated gradient samples in both frame-generation modes, the fixed-size fallback, pipeline-cache retention, and static/changing photometric masks. These are functional checks, not an in-game quality or performance benchmark.

The following station-walk screens used Apple M5/macOS 26.5.1, a 2880x1620 picture, 2x shadows and no shader compilation/skipped draws in the sampled window. Values are medians; real/displayed FPS and GPU samples use the same walking window (present counts650–1025).

| Configuration | Render input | Real FPS | Displayed FPS | GPU busy ms |
| --- | --- | ---: | ---: | ---: |
| Native, frame generation off | 2880x1620 | 75.5 | 75.5 | 13.21 |
| Temporal, frame generation off | 1920x1080 | 82.7 | 82.7 | 11.96 |
| Native + standalone frame generation | 2880x1620 | 42.1 | 84.5 | 23.77 |
| Temporal + frame generation | 1920x1080 | 48.5 | 97.4 | 20.28 |

The first pair is Developer-uncapped with FG off; the second has the60->120 source target with uncapped disabled. Do not treat the two pairs as an identical pacing configuration. Combined mode reduced measured GPU busy time about14.7% against standalone FG in this screen. This is one run per configuration, not a repeatability guarantee: adaptive game timing and live scene content can affect a presented-frame-based route. Some character-edge/disocclusion artifacts remain visible in generated images. Handheld1280x720 within a1920x1080 allocation,30->60 generation and menu fallback were checked separately; those are functional checks, not additional matched performance comparisons. No claim of complete combat/foliage correctness.

Run native tests without game data:

```sh
python3 tools/test_temporal_upscale_settings.py
python3 tools/test_temporal_upscale_gpu.py
python3 tools/test_frame_generation_gpu.py
```

Native GPU tests require a compatible Mac and use synthetic textures only. Raw game captures, shaders, saves and benchmark files remain local and must not be committed.
