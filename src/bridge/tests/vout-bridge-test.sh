set -u
cd "$(dirname "$(readlink -f "$0")")/../../.."
B=build/bridge
RD=$(mktemp -d /tmp/plh-capture/rt.XXXXXX); chmod 700 $RD
export PLH_BRIDGE_DIR=$RD PLH_INPUT_BACKEND=none PLH_VIRTUAL_DISPLAY=1
cnt() { xrandr --listmonitors | grep -c Virtual-plh; }
$B/plh-bridge --runtime-dir $RD > /tmp/plh-capture/bridge.log 2>&1 & BP=$!
for _ in $(seq 50); do [ -S $RD/bridge.sock ] && [ -s $RD/port ] && break; sleep 0.1; done
echo "before: virtual outputs=$(cnt)"
$B/plh-vout-test state
echo "--- A: create, hold 2s, no destroy (connection closes)"
$B/plh-vout-test create -t 2 -f | tee /tmp/plh-capture/A.out | grep -v '^LIST'
sleep 0.5; echo "after A (connection closed): $(cnt)"
echo "--- B: create + destroy"
$B/plh-vout-test create -t 1 -d | grep -E "VOUT|DESTROYED"
sleep 0.3; echo "after B: $(cnt)"
echo "--- C: kill client -9 while holding"
$B/plh-vout-test create -k > /tmp/plh-capture/C.out & CP=$!
sleep 2; echo "during C: $(cnt)"; kill -9 $CP; sleep 0.8; echo "after C: $(cnt)"
echo "--- D: kill bridge while holding"
$B/plh-vout-test create -k > /tmp/plh-capture/D.out & DP=$!
sleep 2; echo "during D: $(cnt)"; kill -9 $BP; sleep 2; echo "after D bridge SIGKILL: $(cnt)"; kill $DP 2>/dev/null
echo "--- E: bridge SIGTERM exit"
$B/plh-bridge --runtime-dir $RD >> /tmp/plh-capture/bridge.log 2>&1 & BP=$!
for _ in $(seq 50); do [ -S $RD/bridge.sock ] && sleep 0.3 && break; sleep 0.1; done
$B/plh-vout-test create -k > /dev/null & EP=$!
sleep 2; echo "during E: $(cnt)"; kill -TERM $BP; sleep 1.5; echo "after E: $(cnt)"; kill $EP 2>/dev/null
rm -rf $RD
