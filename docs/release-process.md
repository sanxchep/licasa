# Release process

The two GitHub Actions workflows use one source commit for the Snap, AppImage,
and Debian package. Launchpad automatic builds are not part of this path.

1. Commit and push the release source to `main`.
2. Start **Build and qualify Linux packages** from the Actions tab on `main`.
   It runs sanitizer and release tests, builds and tests the Snap in GitHub
   Actions, checks the Debian package and AppImage on clean Linux systems,
   uploads the Snap to Store `edge`, verifies that installed revision, and
   publishes a GitHub prerelease with the Snap, AppImage, Debian package, and
   checksums. Its artifacts also include test reports. If a check fails,
   GitHub does not publish the prerelease.
3. Inspect the prerelease, packages, and reports. Copy the successful build
   run ID from its GitHub Actions URL.
4. Start **Approve beta and mark GitHub release latest** and enter that run ID.
   Starting this workflow is the approval to promote beta. It rechecks the
   build, package hashes, embedded Snap source commit, GitHub prerelease, and
   Store `edge` revision. It promotes that revision to beta, installs and tests
   beta, then marks the same GitHub release as Latest.

The `SNAPCRAFT_STORE_CREDENTIALS` secret must be available to the `snap-beta`
GitHub environment. The build workflow uses it only after all package checks
pass. The old Snapcraft push webhook is inactive. Neither workflow runs
automatically on a tag push.
