#!/usr/bin/env bash
# 生成中继用的自签 CA 与服务器证书
# 用法: gen_certs.sh <host-or-ip> [out-dir]
set -euo pipefail

HOST="${1:?usage: gen_certs.sh <relay-host-or-ip> [out-dir]}"
OUT="${2:-tls}"
mkdir -p "$OUT"
cd "$OUT"

if [[ "$HOST" =~ ^[0-9]{1,3}(\.[0-9]{1,3}){3}$ ]]; then
    SAN="IP:$HOST"
else
    SAN="DNS:$HOST"
fi

openssl req -x509 -newkey rsa:2048 -nodes \
    -keyout ca.key -out ca.crt -days 3650 \
    -subj "/CN=KVM-Wire CA" >/dev/null 2>&1

printf "subjectAltName=%s\n" "$SAN" > san.cnf
openssl req -newkey rsa:2048 -nodes \
    -keyout server.key -out server.csr \
    -subj "/CN=$HOST" >/dev/null 2>&1
openssl x509 -req -in server.csr -CA ca.crt -CAkey ca.key \
    -CAcreateserial -out server.crt -days 3650 \
    -extfile san.cnf >/dev/null 2>&1

rm -f server.csr san.cnf
chmod 600 ca.key server.key
chmod 644 ca.crt server.crt
echo "OK: $OUT/{ca.crt,ca.key,server.crt,server.key}"
