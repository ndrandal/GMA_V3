// Manual cross-proposal smoke equivalent for gma-string-id-subscriptions
// phase 2 task 3 (ENC-389). Sends a string-id-shaped subscribe payload
// matching what embassy emits on the saved-scene path, asserts the
// subscribed ack carries `requestId` and at least one value frame
// arrives carrying `requestId` (no `key` field). Bypasses embassy
// entirely; tests gma_v3 in isolation against the embassy wire shape.

'use strict';
const WebSocket = require('ws');

const URL = process.argv[2] || 'ws://localhost:4001';
const ws = new WebSocket(URL);
let sawAck = false;
let sawUpdate = false;
let badShape = false;

const REQ_ID = 'r-NEXO-open';
const STREAM_KEY = 'NEXO';
const FIELD = 'lastPrice';

ws.on('open', () => {
  ws.send(JSON.stringify({
    type: 'subscribe',
    requests: [{ id: REQ_ID, streamKey: STREAM_KEY, field: FIELD }],
  }));
});

ws.on('message', (data) => {
  let msg;
  try { msg = JSON.parse(data.toString()); }
  catch (e) {
    console.error('[probe] unparseable frame:', data.toString().slice(0, 200));
    return;
  }
  if (msg.type === 'subscribed') {
    if (msg.requestId === REQ_ID && !('key' in msg)) sawAck = true;
    else { console.error('[probe] bad ack shape:', JSON.stringify(msg)); badShape = true; }
  } else if (msg.type === 'update') {
    if (msg.requestId === REQ_ID && !('key' in msg)) sawUpdate = true;
    else { console.error('[probe] bad update shape:', JSON.stringify(msg)); badShape = true; }
  } else if (msg.type === 'error') {
    console.error('[probe] gma error frame:', JSON.stringify(msg));
    badShape = true;
  }
});

ws.on('error', (e) => { console.error('[probe] ws error:', e.message); process.exit(2); });

setTimeout(() => {
  ws.close();
  console.log('[probe] sawAck=' + sawAck + ' sawUpdate=' + sawUpdate + ' badShape=' + badShape);
  if (sawAck && sawUpdate && !badShape) { console.log('[probe] PASS'); process.exit(0); }
  console.log('[probe] FAIL');
  process.exit(1);
}, 10000);
