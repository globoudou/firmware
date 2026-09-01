#!/bin/bash
# Deploy V2 plugin to a single parc cam (RAM-only, no bind-mount needed
# for the config file because the plugin reads /tmp/majestic-ae.conf first).
# Preserves the existing V1 as /tmp/hisilicon-v1.so for quick rollback.
#
# Usage: CAM=192.168.1.60 ./deploy_parc.sh
set -euo pipefail
: "${CAM:?CAM=192.168.1.XX required}"
CAMHELPER="/home/claude/zg2332m/tests/optim-nuit-20260831/cam.sh"
BIN=/home/claude/build/hisilicon-v2.so
CONF=/home/claude/build/majestic-ae.conf
run() { CAM=$CAM "$CAMHELPER" "$@"; }

echo "== [$CAM] pre-check =="
run 'df -h /tmp | tail -1; free -m | head -2; echo -n "current plugin cmds: "; echo help | nc -w 2 localhost 4000 2>&1 | head -1'

echo "== [$CAM] push binary =="
run 'rm -f /tmp/hisilicon-v2.b64 /tmp/hisilicon-v2.new'
base64 -w 6000 "$BIN" | while IFS= read -r CHUNK; do
  run "printf '%s\n' '$CHUNK' >> /tmp/hisilicon-v2.b64"
done
run 'base64 -d /tmp/hisilicon-v2.b64 > /tmp/hisilicon-v2.new && rm /tmp/hisilicon-v2.b64'
LOCAL_MD5=$(md5sum "$BIN" | awk '{print $1}')
REMOTE_MD5=$(run 'md5sum /tmp/hisilicon-v2.new' | awk 'NR==1{print $1}')
[ "$LOCAL_MD5" = "$REMOTE_MD5" ] || { echo "MD5 mismatch"; exit 3; }
echo "md5 OK: $LOCAL_MD5"

echo "== [$CAM] push config =="
CONF_B64=$(base64 -w 0 "$CONF")
run "echo '$CONF_B64' | base64 -d > /tmp/majestic-ae.conf.new && mv /tmp/majestic-ae.conf.new /tmp/majestic-ae.conf && wc -c /tmp/majestic-ae.conf"

echo "== [$CAM] install =="
run '
set -e
# backup current plugin (v1) for rollback if not already saved
if [ ! -f /tmp/hisilicon-v1.so ] && [ -f /tmp/usrlib/hisilicon.so ]; then
  cp /tmp/usrlib/hisilicon.so /tmp/hisilicon-v1.so
fi
# put the new one in place
cp /tmp/hisilicon-v2.new /tmp/usrlib/hisilicon.so
chmod 600 /tmp/usrlib/hisilicon.so
ls -la /tmp/hisilicon-v1.so /tmp/usrlib/hisilicon.so /tmp/majestic-ae.conf
'

echo "== [$CAM] restart majestic =="
run '/etc/init.d/S95majestic restart' || true
sleep 10

echo "== [$CAM] verification =="
run '
echo -n "plugin cmds: "; echo help | nc -w 3 localhost 4000
echo -n "ranges     : "; echo gainmax | nc -w 3 localhost 4000
echo -n "expinfo    : "; echo expinfo | nc -w 3 localhost 4000
echo -n "night state: "; wget -qO- http://localhost/night 2>/dev/null || echo "n/a"
'
echo "== [$CAM] done =="
