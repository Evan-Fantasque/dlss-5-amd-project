# test8 implementation and validation

The product source is the tested local 1.7.2 base plus selected 1.7.3 adaptations,
not the complete later experimental feature set. Repository cleanup changes paths
and documentation; the NR product logic remains the tested implementation.

## Submission and ownership

Only the prepared D3D11-to-D3D12 bridge enters phased submission. Its producer
copies/fence are enqueued before inference begins. A COM recording proxy splits
the hash-checked native Record at the capture/wait boundary. Real command lists,
not the proxy, are submitted to the direct queue.

For each pass, submit capture and await its fence, notify HIP, then await both
inference completion and the final worker-retirement marker. The next segment
applies the preceding pass and captures the next one. Submit final apply only
after all required workers retire. Keep output, inputs, command allocators and
lists alive until the outer upscaler actually submits and completes its consumer.

Each new list restores required compute state; buffer state decay at separate
ExecuteCommandLists boundaries is handled explicitly. Native shader, weights and
watchdog thresholds are unchanged. This removes speculative GPU wait spinning
from the tested path at the cost of CPU synchronization and extra submissions.

Boundary mismatches reject before any segment submission. Reserved unpublished
jobs are cancelled for safe worker shutdown. Failures after submission disable NR
and retain resources that might still be in use. Restart after such a failure.

Other retained improvements include source-call based temporal resets, stale-input
and invalid-guide handling, scale-change settling, 1:1-menu gating, optional input
fence waits, balanced 1ms timer requests, and non-invasive CPU timing CSVs.

## Validation summary (RX 9070 XT)

| Case | Result |
|---|---|
| Local 1968x1107, 600 consecutive single-pass jobs | No timeout; actual GPU flags read back after final apply showed zero wait iterations |
| Multipass, size, scale and queue changes | Passed |
| Capture delayed 100ms | HIP not notified prematurely |
| Worker bookkeeping delayed 250ms | Apply waited for final retirement; no spinning |
| Delayed outer consumer | Resources retained until outer fence completion |
| Invalid input/reset, cancellation before any submission | Passed |
| WARP malformed boundaries, overflow, COM/reset contracts | Rejected without GPU submission |
| Retained legacy timeout/recovery path | Passed |
| Game, FG off, 1/3/1 passes | 7,317 NR frames; 7,875 pass jobs in about 286s; zero recorded timeouts |
| Game, FG/NR toggles, 100/75/50%, 1/2/3 passes | 13,631 NR frames; 14,748 pass jobs in about 457s; zero recorded timeouts |

The prior game test had 3 timeouts in 1,253 jobs/about 52s. Current game native
logs sampled zero wait-loop iterations (41 and 79 samples). These are sampled
observations; native counters can lag one job. They do not establish a guarantee
for all games or arbitrarily long sessions. Local strict tests separately cover
the final GPU flags. Original game logs and personal paths are not published.

With FG configured on and a single pass, observed median NR wall times were
27.11ms (100%), 17.07ms (75%) and 11.12ms (50%). Scenes/durations differed. These
are CPU-observed phase timings, not GPU-only inference or displayed FPS. Test
runner work_cpu_ms uses coarse GetTickCount64 and is especially unsuitable for
FPS claims. No GPU debug-layer validation was available.

The standalone cancellation harness is built with `python tools/build.py cancel`;
run a phased case with `--cancel-at 1` or `--cancel-at 3`. It modifies only a generated
test translation unit. --tail-sync inserts a test-only HIP memcpy IAT delay. Neither
probe is linked into the product.

## ABI scope and attribution

Offsets refer only to SHA-256 checked native v0.2.14. Early inference completion
precedes bookkeeping; final retirement must equal the submitted job because -1
sentinels and reused job IDs make unsigned >= unsafe. The wrapper makes no claim
to provide the private neural runtime source or optimize model weights.

Original projects: [AMD integration](https://github.com/MatheusGViana/dlss-5-amd-project),
[PreSR multipass](https://github.com/wilsjo2/OptiScaler-DLSSNR-PreSR-Multipass),
[OptiScaler](https://github.com/optiscaler/OptiScaler),
[private runtime distribution](https://github.com/danielblnc/DLSS-NR-on-AMD).
Licenses remain in LICENSE, src/Licenses and the dependency subdirectories.

Resource-state reasoning follows Microsoft's [resource barrier documentation](https://learn.microsoft.com/en-us/windows/win32/direct3d12/using-resource-barriers-to-synchronize-resource-states-in-direct3d-12).
