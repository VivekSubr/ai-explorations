#!/usr/bin/env bash
# Helpers for running dist_cache.exe (HTTP/3 server) in tests.
set -euo pipefail

# generate_self_signed_cert <out_dir> [cn] [days]
#
# Generates a self-signed TLS cert + key suitable for local HTTP/3 testing.
# Writes <out_dir>/cert.pem and <out_dir>/key.pem.
#
# Echoes the absolute paths of the cert and key (space separated) on stdout
# so callers can capture them, e.g.:
#     read -r CERT KEY < <(generate_self_signed_cert /tmp/dc-certs)
generate_self_signed_cert() {
    local out_dir="${1:?out_dir required}"
    local cn="${2:-localhost}"
    local days="${3:-1}"

    mkdir -p "$out_dir"
    local cert="$out_dir/cert.pem"
    local key="$out_dir/key.pem"

    if ! command -v openssl >/dev/null 2>&1; then
        echo "openssl not found; cannot generate self-signed cert" >&2
        return 1
    fi

    # SAN config so curl --resolve / IP-based clients are happy.
    local san_cfg
    san_cfg=$(mktemp)
    cat >"$san_cfg" <<EOF
[req]
distinguished_name = dn
x509_extensions = v3_req
prompt = no
[dn]
CN = ${cn}
[v3_req]
subjectAltName = @alt
basicConstraints = CA:FALSE
keyUsage = digitalSignature, keyEncipherment
extendedKeyUsage = serverAuth
[alt]
DNS.1 = ${cn}
DNS.2 = localhost
IP.1  = 127.0.0.1
IP.2  = ::1
EOF

    openssl req -x509 -newkey rsa:2048 -nodes \
        -keyout "$key" -out "$cert" \
        -days "$days" \
        -config "$san_cfg" >/dev/null 2>&1

    rm -f "$san_cfg"
    chmod 600 "$key"
    echo "$cert $key"
}

# Example usage when this script is run directly:
if [[ "${BASH_SOURCE[0]}" == "${0}" ]]; then
    out_dir="${1:-./certs}"
    read -r CERT KEY < <(generate_self_signed_cert "$out_dir")
    echo "cert: $CERT"
    echo "key:  $KEY"
fi
