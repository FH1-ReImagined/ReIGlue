# ReIGlue

**ReIGlue: a fork of ReXGlue for Forza recompilations, starting with
[Forza Horizon 1: ReImagined](https://github.com/FH1-ReImagined/ForzaHorizon1-ReImagined).**

[ReXGlue](https://github.com/rexglue/rexglue-sdk) turns Xbox 360 PowerPC code into portable C++ and
runs it on a runtime and a Xenos GPU emulation that come from [Xenia](https://github.com/xenia-project/xenia).
ReIGlue is that SDK plus the fixes and features the Forza Horizon 1 port needed: made for Forza
games, with the general fixes kept apart so any ReXGlue game (and upstream) can take them.

This is not an official ReXGlue project, and it is not affiliated with or endorsed by Microsoft,
Xbox, Playground Games or Turn 10. It contains no game code or data.

## What is in it

`main` is a ReXGlue nightly commit with one commit per change on top. The base is ReXGlue nightly
0.10.0.24, commit `bd833a2` (`nightly-20261002-bd833a2a`). Each commit's subject starts with its
number in the patch series the game repository used to carry (`patches/rexglue-sdk/`), and its
message says what changes and why.

[docs/reiglue/commits.md](docs/reiglue/commits.md) lists every commit and marks it:

- **General** (104): Xenos GPU accuracy (most are Xenia fixes the SDK did not have), performance of
  the GPU emulation and the runtime, input (racing wheels, controller remapping, keyboard and mouse,
  impulse triggers), runtime and platform fixes (Windows, macOS, audio, file system), diagnostics,
  codegen and build.
- **Forza** (33): the scene/HUD split and what builds on it (DLSS, FSR 3, MetalFX, SMAA, live
  graphics settings, ultrawide, HDR with HUD coverage, VR through OpenXR with true stereo), texture
  packs, shader-hash effects such as world ambient occlusion, car thumbnails and FH1's wheel
  layouts. They rely on Forza Horizon 1's shaders, render targets or controls; another game needs
  its own hooks for them.

Issue numbers (`#45`) in commit messages are Forza Horizon 1: ReImagined's issues.

## Tags and versions

A release is a tag: `fh1-<ReXGlue version>-r<n>`, for example `fh1-0.10.0.24-r1` (ReXGlue nightly
0.10.0.24, first ReIGlue revision on it). A game pins a tag; old tags stay, so old game releases still
build. The SDK's own version (`rexglue --version`) follows upstream's scheme and counts the commits
since upstream's `v0.10.0` tag, so it reads `0.10.0.<n>-dev.g<sha>`; the floor that codegen stamps
into a project (`0.10.0`) is the upstream one.

## Building

As upstream, plus one step for the SDL submodule:

```sh
git clone --branch fh1-0.10.0.24-r1 https://github.com/FH1-ReImagined/ReIGlue
cd ReIGlue
git submodule update --init --recursive --depth 1
git -C thirdparty/sdl3 apply "$PWD"/patches/sdl3/*.patch
cmake --preset linux-amd64
cmake --build out/build/linux-amd64 --config Release
```

`patches/sdl3/` holds the one change to SDL (0147: endless haptic effects stay endless on Windows
and macOS), applied to the SDL commit the SDK records. Forza Horizon 1: ReImagined's
`tools/build_rexglue_sdk.sh` does all of this, builds only the GPU plugin (`rexgpu-xenos`), the
runtime (`rexruntime`) and the code generator (`rexglue`), and installs them over an installed
ReXGlue nightly of the same commit, which keeps them ABI-compatible with it.

Some features download pinned, hash-checked files at configure time: the NVIDIA DLSS SDK headers and
loader (`cmake/ForzaDlssSdk.cmake`), AMD FidelityFX FSR 3.1 shaders (`cmake/ForzaFsr3Sdk.cmake`)
and SMAA (`cmake/ForzaSmaa.cmake`). Their licenses are installed with the GPU plugin.

## Following upstream

ReIGlue stays a fork: upstream's history is kept as it is, and `upstream` is the remote for
<https://github.com/rexglue/rexglue-sdk>.

- **A new ReXGlue nightly**: rebase the series onto the nightly's commit, one commit at a time, keeping
  one commit per change (`git rebase --onto <new nightly> <old base> main`). A change upstream has
  taken is dropped; a conflict is fixed in the commit that owns it. Build and test the game on the
  result, then tag `fh1-<new version>-r1`. `main` is rewritten by a rebase; tags are not.
- **A new change**: one commit on `main` with the next free number, a subject `NNNN Area: what`
  and a message that says what changes and why; then a new revision tag (`-r2`, `-r3`, ...) and the
  game moves its pin.
- **Upstream**: general commits are written so they can go to rexglue/rexglue-sdk as they are.

## License and credits

ReXGlue's [LICENSE](LICENSE) (BSD 3-Clause, with Xenia's notice) is unchanged and covers this fork.
[NOTICE](NOTICE) credits the projects ReIGlue builds on: ReXGlue, Xenia and Pinyon Shift, whose
BSD 3-Clause code (ultrawide presentation, the HDR presenter) is adapted here under
[LICENSE-Pinyon-Shift](LICENSE-Pinyon-Shift). Thanks to the ReXGlue team and community for the SDK
this is built on: see [upstream's credits](https://github.com/rexglue/rexglue-sdk#credits).
