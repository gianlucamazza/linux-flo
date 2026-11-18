# linux-flo

Kernel branches for the ASUS/Google Nexus 7 (2013) Wi-Fi (`flo`), on top of
[apq8064-mainline/linux](https://github.com/apq8064-mainline/linux)
(base commit `cc2a6a249`, Linux 7.2 development). The rest of the tree is
upstream Linux, GPL-2.0 (see `COPYING`).

Userland, build wrappers and notes:
[nexus7-flo-linux](https://github.com/gianlucamazza/nexus7-flo-linux).

## Branches

- `flo/main` – the series currently booted on the test tablet (28 patches)
- `flo/<topic>` – work in progress per subsystem (charging, audio, DSI, DSPS,
  USB ID, wcn36xx). Topic branches may be rebased or dropped.
- `flo/rNN`, `flo/*-rNN` – snapshots of earlier test builds
- `flo/base` – the upstream commit everything is based on

## Not intended for upstream

These patches in `flo/main` are local workarounds or experiments:

- `Revert "drm/msm/a3xx: in-stream SMMU TLB invalidation" (local)`
- `ARM: dts: qcom: asus-flo: run the L2 cache at 648 MHz (experimental)`
- `ARM: dts: qcom: apq8064: add the vendor PVS voltage boost to the CPU OPPs`
- `ARM: dts: qcom: apq8064: throttle the CPUs on the passive trip` and
  `ARM: dts: qcom: asus-flo: move the passive CPU trip to 85 C` (local policy)

The other patches are meant to be proposed upstream once they are reviewed
and tested. Nothing has been submitted yet.

## AI assistance

Some patches were written with AI coding assistants; they carry an
`Assisted-by:` trailer. Every patch by Gianluca Mazza carries his
`Signed-off-by:`.
