#!/usr/bin/env bash
# =============================================================================
#  verify_no_network.sh — CI guard for the zero-cloud claim.
#
#  Fails the build if anything that could initiate a network request appears in
#  the source tree. The privacy guarantee is an invariant, not a convention, so
#  it is enforced mechanically on every commit.
# =============================================================================
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
STATUS=0

echo "==> Checking Android manifest for INTERNET permission"
if grep -rq 'android.permission.INTERNET' "${ROOT}/apps/android-compose"; then
  echo "FAIL: INTERNET permission present in Android manifest" >&2
  STATUS=1
fi

echo "==> Checking for networking symbols in app + core sources"
FORBIDDEN='URLSession|NSURLConnection|OkHttp|HttpURLConnection|Retrofit|java\.net\.Socket|curl_easy|getaddrinfo'
if grep -rInE "${FORBIDDEN}" \
     "${ROOT}/core" "${ROOT}/apps" \
     --include='*.swift' --include='*.kt' --include='*.cpp' --include='*.hpp' \
     --exclude-dir=vendor; then
  echo "FAIL: networking API referenced in first-party source" >&2
  STATUS=1
fi

[[ ${STATUS} -eq 0 ]] && echo "==> PASS: no network egress path in first-party code"
exit ${STATUS}
