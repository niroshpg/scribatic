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

# Prebuilt third-party binaries ship inside the app too (sherpa-onnx and ONNX
# Runtime, ADR-009). A source grep cannot see into them, so check what they
# import instead: a library that never links socket or DNS symbols cannot open
# a connection by itself. Only checked when staged (`make fetch-deps`).
PREBUILT="${ROOT}/core/engine/vendor-bin/sherpa-onnx"
NET_SYMBOLS='^_?(socket|connect|getaddrinfo|gethostbyname|SSL_connect|curl_easy_init|nw_connection_create)$'
if [[ -d "${PREBUILT}" ]]; then
  echo "==> Checking prebuilt libraries for network symbol imports"
  NM="$(command -v llvm-nm || command -v nm)"
  while IFS= read -r -d '' lib; do
    # An nm that cannot read the file (GNU nm on a foreign-arch ELF) must fail
    # the check, not pass it with an empty symbol list.
    if ! imports="$("${NM}" -u "${lib}" 2>/dev/null)"; then
      echo "FAIL: cannot inspect ${lib#${ROOT}/} with ${NM}; install llvm-nm" >&2
      STATUS=1
    elif awk '{print $NF}' <<<"${imports}" | grep -qE "${NET_SYMBOLS}"; then
      echo "FAIL: ${lib#${ROOT}/} imports networking symbols" >&2
      STATUS=1
    fi
  done < <(find "${PREBUILT}" \( -name '*.so' -o -name '*.dylib' -o -path '*.framework/SherpaOnnxC' \) -type f -print0)
fi

[[ ${STATUS} -eq 0 ]] && echo "==> PASS: no network egress path in first-party code or staged prebuilts"
exit ${STATUS}
