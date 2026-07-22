# Releasing EMlog

This project is set up so a Git tag is the release trigger.

## Release flow

1. Make sure the branch you want to release is green locally.
   Run:
   ```sh
   ./utils/run_pipeline.sh
   ```

2. Bump `VERSION` if the release version changed.

3. Merge the release candidate branch into `master`.

4. Update local `master`.
   Run:
   ```sh
   git checkout master
   git pull --ff-only origin master
   ```

5. Create an annotated tag that matches `VERSION`.
   Example for `VERSION=1.2.0`:
   ```sh
   git tag -a v1.2.0 -m "EMlog v1.2.0"
   ```

6. Push `master`, then push the tag.
   Run:
   ```sh
   git push origin master
   git push origin v1.2.0
   ```

7. GitHub Actions `release.yml` will:
   - run the full `Quality` gate (build, compiler portability, unit tests +
     coverage, integration test, sanitizers, ThreadSanitizer, package smoke
     test) via `workflow_call`
   - build the hardened, release-profile Debian package
   - generate `SHA256SUMS` and a build-provenance attestation
   - publish a GitHub Release with the artifacts attached

## Important rules

- The tag must match `VERSION` exactly.
  If `VERSION` is `1.2.0`, the tag must be `v1.2.0`.

- Release tags should always be annotated tags, not lightweight tags.

- Do not tag from a branch that is not the exact code you want released.

- A release only publishes if the `Quality` gate passes. There is no way to
  bypass it from a tag push.

## Published release artifacts

Each GitHub Release publishes:

- `emlog_<version>_<arch>.deb`
- `SHA256SUMS`
- a build-provenance attestation (verifiable with `gh attestation verify`)
