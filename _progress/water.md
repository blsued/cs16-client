# Water worker progress (worker/r1-rain-water-snow)

## Jobs
1. 113 FPS fix: batch 324 per-face GL_TRIANGLE_FAN draws -> 1 glDrawArrays(GL_TRIANGLES).
2. W4: ripple-warped reflection feel + ring-term fix.
3. W5: delete stale stub comments.

## Plan
- EnsureBuilt: triangulate each turb fan into a triangle list at build time. Keep
  per-texture grouping (BSP turb faces usually share the same liquid texture, but
  group by texSlot to keep a single draw when possible; fall back to a few draws,
  one per distinct texture slot). Store contiguous ranges per texSlot.
- Add per-vertex attribute a_faceCenter (vec2 world XY centroid of the source face)
  for the rain ring center so a single batched draw still gets a stable per-face ring.
- DrawWater: bind VAO once, iterate the small per-texture batch list -> one
  glDrawArrays per distinct texture (de_aztec = 1 texture => 1 draw call).
- Restore TMU0 after the pass (symmetric unbind / comment).
- Shader: warp the skyReflect sample by animated normal so reflection wobbles
  (already partly there via Nref); add small refraction tint shift with normal+depth.
  Fix ring center to use a_faceCenter so rings are concentric per face.

## Status
- [done] header: WaterFace -> WaterBatch (firstVert/vertCount/texSlot per texture run).
- [done] EnsureBuilt: triangulate fans -> triangle list; sort faces by texSlot
  (insertion sort); emit contiguous runs; build batch table. Vertex stride
  8 -> 10 floats (added a_faceCenter vec2 world-XY centroid). Count pass mirrors
  64-edge cap so VBO sizing matches emission exactly.
- [done] DrawWater: one glDrawArrays(GL_TRIANGLES) per batch (de_aztec single
  water texture => 1 draw call vs 324 fans). TMU0 restored (BindTextureSlot(0,0)).
- [done] shader VS: pass a_faceCenter -> v_faceCenter.
- [done] shader FS ring fix: ring radius + outward dir both from v_faceCenter in
  world units (was fract() grid-cell chop). Slow center wobble.
- [done] shader FS reflection: second ripple octave jitters R.z -> mirrored sky
  breaks into shifting bands (screen-space-style perturb, no FBO).
- [done] shader FS refraction: UV warp + body tint scale with view depth.
- [done] W5: no literal stub/scaffold comments remained (iter1 already rewrote
  them into accurate descriptions); nothing to delete.
- Build/launch: NOT run (separate build agent).
