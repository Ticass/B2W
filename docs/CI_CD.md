# Automated builds and releases

All builds and publication run on GitHub-hosted `windows-2022` and
`ubuntu-24.04` runners. No local runner or running PC is required.

Every branch push (except generated `release/nightly/**` branches) and every pull
request runs **Bleeding edge builds**. Each run builds the native T4 extractor,
vendored T6 tools and audio decoder from source, runs the Windows regression
suite, packages Windows, and packages and checks the Linux frontend using that
same Windows worker. Download `windows-package` and `linux-package` from the
run's **Artifacts** section on the [Actions page](https://github.com/Ticass/B2W/actions).
Artifacts remain available for 14 days; downloading them requires GitHub login.
Each push gets its own run; later pushes do not cancel earlier builds. Pull
request builds test GitHub's merge commit.

**Nightly and version releases** runs daily at **06:17 UTC** (02:17 in Toronto
during daylight saving time, 01:17 during standard time). GitHub can delay
scheduled runs. The scheduled event's default-branch commit is the cutoff:
the workflow creates `release/nightly/YYYY-MM-DD-RUN_ID` at that commit and
checks out the exact commit for both packages. Later commits go into the next
nightly, even if today's build is still running. The branch is never advanced
by the workflow. A rerun reuses the original branch, commit and release name.
Manual nightly runs are supported from `main` with **Run workflow**.

After all package checks pass, the workflow publishes a dated prerelease
`nightly-YYYY-MM-DD-RUN_ID` on the [Releases page](https://github.com/Ticass/B2W/releases).
Each release contains Windows and Linux packages, SHA-256 checksums and
`build-info.json` with the source commit and Actions run. Nightlies do not
replace the latest stable release. Snapshot branches and nightly releases are
retained; remove older ones manually if desired. A failed build leaves its
snapshot branch for diagnosis and publishes no release. Publishing first
uploads assets to a draft, so an upload failure on the first attempt leaves
a draft that can be completed by rerunning the workflow.

For a stable release, update `pyproject.toml` and `CHANGELOG.md`, commit those
changes, then push a version tag such as `v0.2.15`. The same tested packaging
pipeline publishes that tag as the latest stable release. Nightlies retain
the source version and are distinguished by their release tag and provenance.

No custom secrets are required. The snapshot and publish jobs request
`contents: write` on `GITHUB_TOKEN`; build jobs have read access and run without
persisted Git credentials. Repository rules must allow Actions to create
`release/nightly/**` branches, release tags and releases. Scheduled workflows
become active once these files are on the default branch and Actions is enabled.
The old **Build Linux release** workflow remains available to repair historical
releases that already contain a Windows ZIP.

Native upstream commits and dependency submodules are pinned to the snapshots
in `docs/USAGE.md`; Premake's executable checksum is verified. Packages include
the corresponding native source archives and license notices. CI smoke tests
check packaging, launchers, schemas and native tool presence; conversion and
gameplay verification still require game installations and playtesting.
