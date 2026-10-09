# Automated builds and releases

All builds and publication run on GitHub-hosted `windows-2022` and
`ubuntu-24.04` runners. No local runner or running PC is required.

Every branch push (except generated `release/nightly/**` branches) and every pull
request runs **Bleeding edge builds**. Native T4/T6 tools and the audio decoder
are built from source on a cache miss. Subsequent runs with identical native
sources and build scripts reuse the verified binaries, the T4 weapon schema,
and their corresponding source archives. Cache keys include the upstream patch,
vendored T6 source/build definitions, and decoder/build/archive scripts; partial
cache matches are not used. Python changes still run the regression suite and
create fresh Windows and Linux packages. Native compilation uses the runner's
CPU count with a shared compiler-process budget across concurrent projects;
`WAW2BO2_BUILD_WORKERS` can limit this budget. GitHub-hosted builds use the
runner's CPU cores rather than the desktop PC's cores.

Each run executes the Windows regression
suite, packages Windows, and packages and checks the Linux frontend using that
same Windows worker. Download `windows-package` and `linux-package` from the
run's **Artifacts** section on the [Actions page](https://github.com/Ticass/B2W/actions).
Artifacts remain available for 14 days; downloading them requires GitHub login.
Each push gets its own run; later pushes do not cancel earlier builds. Pull
request builds test GitHub's merge commit.

**Nightly and version releases** runs daily at **06:17 UTC** (02:17 in Toronto
during daylight saving time, 01:17 during standard time). GitHub can delay
scheduled runs.

Daily work is merged into the day's release branch, `release/YYYY-MM-DD`. The
nightly builds the newest such branch (or `main` when none exists). Its commit
at the start of the run is the cutoff: the workflow creates
`release/nightly/YYYY-MM-DD-RUN_ID` at that commit and checks out the exact
commit for both packages. A rerun reuses the original snapshot branch, commit
and release name. Manual nightly runs are supported from `main` with **Run
workflow**.

After all package checks pass, the workflow publishes a dated prerelease
`nightly-YYYY-MM-DD-RUN_ID` on the [Releases page](https://github.com/Ticass/B2W/releases).
Each release contains Windows and Linux packages, SHA-256 checksums and
`build-info.json` with the source commit and Actions run. Nightlies do not
replace the latest stable release. Snapshot branches and nightly releases are
retained; remove older ones manually if desired. A failed build leaves its
snapshot branch for diagnosis and publishes no release. Publishing first
uploads assets to a draft, so an upload failure on the first attempt leaves
a draft that can be completed by rerunning the workflow.

Once the nightly is published, the workflow rolls the release branch over:

1. The snapshot commit is merged into `main`. Only what the nightly built
   reaches `main`.
2. `release/<Toronto date of the run>` is created from the updated `main`. This
   is the branch to merge into that day.
3. Commits pushed to the old release branch after the cutoff were not in the
   nightly; they are merged into the new branch instead of `main`.
4. The old release branch is deleted.

A failed build publishes nothing and changes no branch, so work keeps going
into the same release branch and the next nightly picks it up. A merge conflict
stops the rollover with the old branch kept and no new branch created; resolve
it by hand, then rerun the failed job. Pushes made with `GITHUB_TOKEN` start no
other workflows, so the merge into `main` produces no extra build or testing
announcement.

For a stable release, update `pyproject.toml` and `CHANGELOG.md`, commit those
changes, then push a version tag such as `v0.2.15`. The same tested packaging
pipeline publishes that tag as the latest stable release. Nightlies retain
the source version and are distinguished by their release tag and provenance.

No custom secrets are required. The snapshot, publish and rollover jobs request
`contents: write` on `GITHUB_TOKEN`; build jobs have read access and run without
persisted Git credentials. Repository rules must allow Actions to create and
delete `release/**` branches, merge into `main`, and create release tags and
releases. `GITHUB_TOKEN` cannot write commits that change `.github/workflows`,
so a release branch that edits a workflow fails to merge. To merge those
automatically too, add a `RELEASE_BRANCH_TOKEN` Actions secret: a fine-grained
token for this repository with **Contents** and **Workflows** write access. Scheduled workflows
become active once these files are on the default branch and Actions is enabled.
The old **Build Linux release** workflow remains available to repair historical
releases that already contain a Windows ZIP.

Native upstream commits and dependency submodules are pinned to the snapshots
in `docs/USAGE.md`; Premake's executable checksum is verified. Packages include
the corresponding native source archives and license notices. CI smoke tests
check packaging, launchers, schemas and native tool presence; conversion and
gameplay verification still require game installations and playtesting.
