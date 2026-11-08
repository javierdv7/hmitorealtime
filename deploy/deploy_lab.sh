#!/usr/bin/env bash
# Sync agent + HMI + schema to the CM4 ("lab") and (re)start the HMI service.
set -euo pipefail
HOST="${LAB_HOST:-lab}"
DEST="${LAB_DIR:-/mnt/external/edge-rules}"
cd "$(dirname "$0")/.."

ssh "$HOST" "mkdir -p $DEST"
rsync -az --delete --exclude .venv --exclude __pycache__ agent hmi schema deploy "$HOST:$DEST/"
ssh "$HOST" bash -s <<REMOTE
set -e
cd $DEST
[ -d .venv ] || python3 -m venv .venv
.venv/bin/pip install -q -r agent/requirements.txt
# broker: LAN listener (idempotent)
if [ ! -f /etc/mosquitto/conf.d/edge.conf ]; then
  sudo apt-get install -y mosquitto mosquitto-clients >/dev/null
  sudo cp deploy/mosquitto-edge.conf /etc/mosquitto/conf.d/edge.conf
  sudo systemctl enable --now mosquitto && sudo systemctl restart mosquitto
fi
sudo cp deploy/edge-hmi.service /etc/systemd/system/edge-hmi.service
sudo systemctl daemon-reload
sudo systemctl enable --now edge-hmi
sudo systemctl restart edge-hmi
sleep 1
systemctl is-active edge-hmi mosquitto
REMOTE
echo "HMI:   http://$(ssh "$HOST" hostname -I | awk '{print $1}'):8090"
echo "Agent: ssh -t $HOST 'cd $DEST && .venv/bin/python agent/edge_agent.py'"
