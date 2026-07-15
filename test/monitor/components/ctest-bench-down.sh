#!/usr/bin/env bash
# ctest FIXTURES_CLEANUP: tear down the smbench container. The behavioral image
# and the host-side binary caches persist (cheap to re-provision). Cleanup never
# fails the suite.
docker rm -f smbench >/dev/null 2>&1 || true
exit 0
