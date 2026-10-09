# Automated builds and releases

All builds and publication run on GitHub-hosted `windows-2022` and
`ubuntu-24.04` runners. No local runner or running PC is required.

The flow follows [t3code](https://github.com/pingdotgg/t3code)'s pipeline:
fixes are tested on their own branches, `main` only receives tested work, and
releases are cut from `main` on a schedule.

## Daily flow

1. Each fix lives on its own branch. Every push to it runs **Preview builds**,
   which packages Windows and Linux and posts a testing build announcement on
   Discord.
2. Once a fix is tested, merge it into `main` by hand. `main` never gets a
   preview build or a testing announcement.
3. At **midnight Toronto time**, **Nightly and version releases** cuts a nightly
   from `main`, publishes it as **Nightly YYYY-MM-DD** and announces it on
   Discord. A night with no new commits on `main` since the last nightly
   publishes nothing.
4. For a stable release, update `pyproject.toml` and `CHANGELOG.md` on `main`,
   then push a version tag such as `v0.2.15`.

## Preview builds

**Preview builds** (`ci.yml`) runs for every push to any branch except `main`.
It can also be started by hand on a fix branch (the Discord investigation does
this for its draft PR branches); a manual run on `main` does nothing. Native
T4/T6 tools and the audio decoder are built from source on a cache miss.
Subsequent runs with identical native sources and build scripts reuse the
verified binaries, the T4 weapon schema, and their corresponding source
archives. Cache keys include the upstream patch, vendored T6 source/build
definitions, and decoder/build/archive scripts; partial cache matches are not
used. Python changes still run the regression suite and create fresh Windows
and Linux packages. Native compilation uses the runner's CPU count with a
shared compiler-process budget across concurrent projects;
`WAW2BO2_BUILD_WORKERS` can limit this budget.

Each run executes the Windows regression suite, packages Windows, and packages
and checks the Linux frontend using that same Windows worker. Download
`windows-package` and `linux-package` from the run's **Artifacts** section on
the [Actions page](https://github.com/Ticass/B2W/actions). Artifacts remain
available for 14 days; downloading them requires GitHub login. Each push gets
its own run; later pushes do not cancel earlier builds.

## Nightly releases

The schedule has two UTC times, 04:00 and 05:00. The `resolve` job keeps the one
that is midnight in Toronto (daylight saving or standard time) and skips the
other. GitHub can delay scheduled runs.

The nightly builds `main`'s commit at the time of the scheduled event; later
commits go into the next nightly, and a rerun builds the same commit. When
`main` has no commits since the latest published nightly, the run stops
without building. **Run workflow** on `main` starts a nightly by hand, built
even without new commits; it refuses any other branch.

After all package checks pass, the workflow publishes the prerelease
`nightly-YYYY-MM-DD-RUN_ID`, titled **Nightly YYYY-MM-DD**, on the
[Releases page](https://github.com/Ticass/B2W/releases). Its tag pins the
built commit; there are no snapshot or release branches. Each release contains
Windows and Linux packages, SHA-256 checksums and `build-info.json` with the
source commit and Actions run, and its notes link the changes since the
previous nightly. Nightlies do not replace the latest stable release. A failed
build publishes nothing. Publishing first uploads assets to a draft, so an
upload failure on the first attempt leaves a draft that can be completed by
rerunning the workflow. Once published, the nightly is announced on Discord
(see `docs/RELEASE_ANNOUNCEMENTS.md`).

## Stable releases

Push a version tag such as `v0.2.15` after updating `pyproject.toml` and
`CHANGELOG.md` on `main`. The same tested packaging pipeline publishes that tag
as the latest stable release. Nightlies retain the source version and are
distinguished by their release tag and provenance.

## Permissions

No custom secrets are required. The publish job requests `contents: write` on
`GITHUB_TOKEN`; build jobs have read access and run without persisted Git
credentials. Repository rules must allow Actions to create release tags and
releases. Scheduled workflows become active once these files are on the
default branch and Actions is enabled. The old **Build Linux release** workflow
remains available to repair historical releases that already contain a Windows
ZIP.

Native upstream commits and dependency submodules are pinned to the snapshots
in `docs/USAGE.md`; Premake's executable checksum is verified. Packages include
the corresponding native source archives and license notices. CI smoke tests
check packaging, launchers, schemas and native tool presence; conversion and
gameplay verification still require game installations and playtesting.
