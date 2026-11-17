# Ocarina of Time: bounded ASTC decode scratch

This focused port follows upstream Eglantine commit
[`54c034ad29`](https://github.com/Eglantine-Community/Eglantine/commit/54c034ad29d4105a869364f4bc1380a5d83533db),
originally `0660154d14aadc08bcf9c7b16296d7610dbc7189`. The user explicitly authorized this
port without the full upstream merge, which has 404 conflicted paths against the starting tree.

## Problem and change

GPU ASTC-to-BC transcoding uses a temporary RGBA8 image. Previously, completed images were
recycled only at the frame boundary. A long frame decoding many textures could allocate new
scratch images even after earlier GPU decodes had finished. Discrete GPUs also attempted whole
array allocations and only reduced their size after allocation failed; a driver that instead
spilled memory into host RAM would not take that fallback.

The port reclaims completed scratch before each transcode and keeps frame-boundary reclamation.
Arrays start with at most 64 MiB of calculated RGBA mip data per batch, or one layer when a
single layer is larger. Before a new input staging allocation, the runtime waits for enough
previous transcodes to complete to keep pending decode data plus the next batch within 512 MiB.
An individual layer larger than 512 MiB is allowed alone, with older pending work drained first.

The bound is calculated image data, not a limit on total VRAM: allocator alignment/block slack,
the existing free scratch pool (up to 256 MiB), BC output staging and game images are separate.
Existing allocation fallback can reduce the selected batch further.

## Adaptation to Lichii

The pressure wait is in `PrepareImageUpload`, called before obtaining input staging. Putting
the wait directly inside the upstream transcode entry can submit the tick that owns an input
map before its decode commands are recorded. Moving it earlier avoids retiring that input
before use or making it eligible for staging-pool reuse during a later allocation.

The port retains Lichii's physical host mip limits, `R8G8B8A8_UNORM` storage format, array-tail
layer counts, overwrite barriers between batches, final-batch-only decoder submission, deferred
encoded output buffer and final copy offsets. It does not replace the newer upload loop with
the older upstream loop. No experimental environment switch is introduced.

## Validation

Regression cases cover odd and one-dimensional mip chains, smaller host mip counts, 1365x1365
arrays with 90 layers, layers larger than 64 MiB, a layer larger than 512 MiB, exact pending
budget boundaries and overflow-safe budget comparisons. Existing host-mip and eviction-budget
tests are included in the focused test run.

On 17 November 2026 the focused sources compiled with portable LLVM-MinGW Clang 23.1.5,
C++20, `-Wall -Wextra -Werror`, and Catch2 3.8.1. All 129 assertions in 11 test cases passed.
`git diff --check` also passed. This was a standalone compilation of the three regression-test
translation units and their helper headers, not the complete renderer or CMake test target.

A full Vulkan renderer build and runtime comparison remain required before calling this a
validated game fix. In particular, test BC1 and BC3 textures, partial final array batches,
allocation failure, and image correctness during the same Ocarina traversal. Compare host spill,
frame-time tails and repeated image creation; do not assume that bounding scratch alone fixes
all texture eviction or improves every scene.
