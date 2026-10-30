#!/usr/bin/env bash
# Make a FIDO attestation key and certificate for the KGFIDO_ATTEST_KEY /
# KGFIDO_ATTEST_CERT build variables, with openssl: a P-256 key and an X.509
# v3 certificate as FIDO requires (subject OU "Authenticator Attestation",
# CA:FALSE, the AAGUID in extension 1.3.6.1.4.1.45724.1.1.4), signed by a
# sample root CA made alongside (or by an existing one: CA_KEY / CA_CERT).
#   ./tools/fido_attest.sh <aaguid> [out_dir]    # out_dir: example/002/attest
#   ORG="Example Inc." ./tools/fido_attest.sh $(uuidgen)
# Writes attest_key.hex (the build input; SECRET), attest_cert.der, and the
# PEM files. Every key built with this pair shares it, which is what batch
# attestation means: use one pair per batch of keys, never one per key.
set -euo pipefail
source "$(dirname "$0")/env.sh"

aaguid="$(echo "${1:?usage: fido_attest.sh <aaguid> [out_dir]}" | tr -d '-' | tr 'A-F' 'a-f')"
[[ "$aaguid" =~ ^[0-9a-f]{32}$ ]] || { echo "aaguid must be 32 hex digits" >&2; exit 1; }
out="${2:-$KGFIDO_DIR/../attest}"
ORG="${ORG:-KagigataDisk sample}"
DAYS="${DAYS:-7300}"
mkdir -p "$out"
umask 077
cd "$out"

if [ -z "${CA_KEY:-}" ]; then
  CA_KEY=ca_key.pem
  CA_CERT=ca_cert.pem
  if [ ! -f "$CA_KEY" ]; then
    openssl ecparam -name prime256v1 -genkey -noout -out "$CA_KEY"
    openssl req -x509 -new -key "$CA_KEY" -sha256 -days "$DAYS" \
      -subj "/C=JP/O=$ORG/CN=$ORG FIDO Root CA" -out "$CA_CERT"
  fi
fi

openssl ecparam -name prime256v1 -genkey -noout -out attest_key.pem
openssl req -new -key attest_key.pem \
  -subj "/C=JP/O=$ORG/OU=Authenticator Attestation/CN=KagigataDisk FIDO2" -out attest.csr
cat > attest_ext.cnf <<EXT
basicConstraints=critical,CA:FALSE
1.3.6.1.4.1.45724.1.1.4=ASN1:FORMAT:HEX,OCTETSTRING:$aaguid
EXT
openssl x509 -req -in attest.csr -CA "$CA_CERT" -CAkey "$CA_KEY" -CAcreateserial \
  -days "$DAYS" -sha256 -extfile attest_ext.cnf -out attest_cert.pem 2>/dev/null
openssl x509 -in attest_cert.pem -outform DER -out attest_cert.der
rm -f attest.csr attest_ext.cnf

# The private scalar as 64 hex digits (openssl prints it with a leading 00
# when the top bit is set).
priv="$(openssl ec -in attest_key.pem -noout -text 2>/dev/null |
  sed -n '/^priv:/,/^pub:/p' | sed '1d;$d' | tr -d ' :\n')"
priv="$(printf '%064s' "${priv: -64}" | tr ' ' 0)"
echo "$priv" > attest_key.hex

echo "wrote $(pwd)/attest_key.hex (secret) and $(pwd)/attest_cert.der; flash with"
echo "  ./tools/kagigata_flash.sh 002 -DKGFIDO_AAGUID=$aaguid \\"
echo "      -DKGFIDO_ATTEST_KEY=$(realpath attest_key.hex) -DKGFIDO_ATTEST_CERT=$(realpath attest_cert.der)"
