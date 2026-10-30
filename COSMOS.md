# Cosmos

Cosmos is the Wine engine of [Cosmopolitan](https://github.com/cyber-prinzesse/Cosmopolitan). It is built on the `cosmos`
branch of this fork: Highball's recipe and patch series (upstream `main` of gauthierpiarrette/highball-engine) plus
Cosmopolitan's own patches, built by `.github/workflows/build.yml` with `cosmos_revision` set.

- Tarball: `cosmos-<wine version>-r<N>.tar.gz` — Highball's layout (`engine/`, `renderers/`, `frameworks/`, `tools/`) with
  `wswine.bundle -> engine` for Cosmopolitan, and `COSMOS-BUILD.txt` naming the source pin, the patches and the commit.
- Wine source: CodeWeavers' CrossOver sources (LGPL-2.1-or-later), pinned by SHA-256 in `inputs.json`. The complete
  corresponding source of any Cosmos release is that tarball plus the `patches/` folder at the release's commit.
- Cosmopolitan's patches so far: `patches/0028` (a fault on a thread Wine didn't create ends the program instead of
  hanging it), also offered upstream as highball-engine PR #24.
- Credit: CodeWeavers, Highball (gauthierpiarrette) and everyone named in README.md's "Provenance and credit".

Keeping up with Highball: merge `upstream/main` into `cosmos`, rebuild, bump the revision.
