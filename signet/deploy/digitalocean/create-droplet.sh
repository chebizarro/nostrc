#!/usr/bin/env bash
# Operator-only: this creates billable resources. It does not install secrets.
set -euo pipefail
[[ ${1:-} == --create ]] || { echo 'Refusing cloud mutation without --create (see deploy/README.md)' >&2; exit 2; }
: "${DO_REGION:?}" "${SIGNET_HOST:?}" "${DO_SSH_KEY_ID:?}" "${DO_VPC_ID:?}"
: "${OPERATOR_CIDR:?}" "${OPERATOR_SSH_PUBLIC_KEY_FILE:?}"
export DO_VOLUME_NAME=${DO_VOLUME_NAME:-signet-data}
export OPERATOR_SSH_PUBLIC_KEY_FILE
here=$(cd -- "$(dirname -- "$0")" && pwd)
command -v doctl >/dev/null
python3 - "$OPERATOR_CIDR" "$DO_VPC_ID" "$SIGNET_HOST" "$DO_VOLUME_NAME" <<'PY'
import ipaddress,re,sys,uuid,yaml
n=ipaddress.ip_network(sys.argv[1],strict=True)
assert n.version == 4 and n.prefixlen > 0, 'operator CIDR must be restricted IPv4'
uuid.UUID(sys.argv[2])
assert all(re.fullmatch('[a-z0-9-]+', x) for x in sys.argv[3:]), 'invalid host/volume name'
PY
# Check supported flags before any resource is created.
doctl compute droplet create --help | grep -q -- --volumes
doctl compute firewall create --help | grep -q -- --tag-names
umask 077
receipt=${SIGNET_PROVISION_RECEIPT:-signet-provision-$(date -u +%Y%m%dT%H%M%SZ)}
mkdir "$receipt"
echo 'Resource IDs and failures are retained. No automatic deletion on failure.' >&2
doctl compute tag create signet > "$receipt/tag.txt"
# Tag-scoped firewall exists BEFORE either the volume or Droplet.
doctl compute firewall create --name signet-fw --tag-names signet \
  --inbound-rules "protocol:tcp,ports:22,address:$OPERATOR_CIDR" \
  --outbound-rules 'protocol:tcp,ports:443,address:0.0.0.0/0 protocol:udp,ports:53,address:0.0.0.0/0 protocol:tcp,ports:53,address:0.0.0.0/0 protocol:udp,ports:123,address:0.0.0.0/0' \
  --output json > "$receipt/firewall.json"
doctl compute volume create "$DO_VOLUME_NAME" --region "$DO_REGION" --size 20GiB \
  --fs-type ext4 --output json > "$receipt/volume.json"
DO_VOLUME_ID=$(python3 -c 'import json,sys; print(json.load(open(sys.argv[1]))[0]["id"])' "$receipt/volume.json")
export DO_VOLUME_ID
python3 "$here/render-cloud-init.py" > "$receipt/cloud-init.yaml"
if command -v cloud-init >/dev/null; then cloud-init schema --config-file "$receipt/cloud-init.yaml"; fi
doctl compute droplet create "$SIGNET_HOST" --image ubuntu-24-04-x64 --size s-1vcpu-2gb \
  --region "$DO_REGION" --ssh-keys "$DO_SSH_KEY_ID" --vpc-uuid "$DO_VPC_ID" \
  --volumes "$DO_VOLUME_ID" --tag-names signet --enable-monitoring \
  --user-data-file "$receipt/cloud-init.yaml" --wait --output json > "$receipt/droplet.json"
echo "Created resources; inspect $receipt and cloud-init status before key ceremony."
