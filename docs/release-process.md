# Release process

The two GitHub Actions workflows use one source commit for the Snap, AppImage,
and Debian package. Launchpad automatic builds are not part of this path.

1. Commit and push the release source to `main`.
2. Start **Build and qualify Linux packages** from the Actions tab on `main`.
   It runs sanitizer and release tests, builds the Snap in GitHub Actions,
   installs that file under strict confinement, checks the Debian package and
   AppImage on clean Linux systems, and uploads the checked Snap to Store
   `edge`. Its artifacts include all three packages, checksums, and test
   reports. If any check fails, beta and the GitHub release remain unchanged.
3. Confirm that the build workflow succeeded and inspect its packages and
   reports. Copy the successful run ID from its GitHub Actions URL.
4. Start **Promote checked Snap and publish release** on the same commit and
   enter that run ID. Starting this workflow is the approval to promote beta.
   It rechecks the run, package hashes, embedded Snap source commit, and Store
   `edge` revision. It promotes that revision to beta, installs and tests beta,
   then publishes the GitHub release with the Snap, AppImage, Debian package,
   and checksum files.

The `SNAPCRAFT_STORE_CREDENTIALS` secret must be available to the `snap-beta`
GitHub environment. The build workflow uses it only after all package checks
pass. Neither workflow runs automatically on a tag push.
