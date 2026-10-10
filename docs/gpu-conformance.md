# GPU conformance

Every compute kernel is diffed against a CPU reference implementation whose
floating-point behaviour is pinned, on the RTX 4090 in the CI machine, on every
push. The reference is compiled with contraction disabled so that it gives the
same answer regardless of which host compiler built it. A referee that moves
cannot referee anything.

CI runs on a self-hosted Windows workstation, because a hosted runner has no
discrete GPU and therefore cannot execute the one suite this project's
correctness argument depends on.

| Vendor | Coverage |
| --- | --- |
| NVIDIA | RTX 4090, discrete, Vulkan 1.4.351. Every push. |
| AMD | **No coverage.** The Radeon integrated graphics in the CI machine's Ryzen 9 7950X is not supported and was dropped from CI on 2026-09-22, and there is no discrete AMD card. |
| Intel | **No coverage.** There is no Intel GPU in the machine and one cannot be added to it. |

The AMD and Intel gaps are real and are recorded rather than papered over. One
of the three target vendors gets genuine driver coverage on every commit. AMD
and Intel get none, and a kernel that behaves differently on either would not
be caught until somebody runs it on one. A discrete AMD card and an Intel
device in the conformance machine are the fix, and until that happens this
table is the honest statement of what is verified.

macOS through MoltenVK is out of scope until there is something to render.
