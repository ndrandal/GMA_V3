#!/usr/bin/env node
// smoke.js — GMA_V3 end-to-end smoke test client
//
// Connects to the GMA WS server, subscribes to a comprehensive set of
// pipeline patterns, and verifies that every key receives at least one
// update within the configured duration.
//
// Usage:
//   node smoke.js [--url ws://localhost:8080] [--duration 30] [--verbose]

'use strict';

const WebSocket = require('ws');

// ---------------------------------------------------------------------------
// Argument parsing
// ---------------------------------------------------------------------------
const args = process.argv.slice(2);
function argVal(flag, fallback) {
  const idx = args.indexOf(flag);
  return idx !== -1 && idx + 1 < args.length ? args[idx + 1] : fallback;
}
const URL      = argVal('--url', 'ws://localhost:8080');
const DURATION = parseInt(argVal('--duration', '30'), 10);
const VERBOSE  = args.includes('--verbose');

// ---------------------------------------------------------------------------
// Subscribe requests — tiers 1-6
// ---------------------------------------------------------------------------
const REQUESTS = [
  // Tier 1: Direct field subscriptions (raw tick + TA)
  { key: 1,  symbol: 'NEXO',  field: 'lastPrice',       label: 'NEXO/lastPrice' },
  { key: 2,  symbol: 'NEXO',  field: 'volume',          label: 'NEXO/volume' },
  { key: 3,  symbol: 'NEXO',  field: 'sma_5',           label: 'NEXO/sma_5' },
  { key: 4,  symbol: 'NEXO',  field: 'sma_20',          label: 'NEXO/sma_20' },
  { key: 5,  symbol: 'NEXO',  field: 'ema_12',          label: 'NEXO/ema_12' },
  { key: 6,  symbol: 'NEXO',  field: 'rsi_14',          label: 'NEXO/rsi_14' },
  { key: 7,  symbol: 'NEXO',  field: 'macd_line',       label: 'NEXO/macd_line' },
  { key: 8,  symbol: 'NEXO',  field: 'bollinger_upper', label: 'NEXO/bollinger_upper' },
  { key: 9,  symbol: 'VALT',  field: 'lastPrice',       label: 'VALT/lastPrice' },
  { key: 10, symbol: 'BLITZ', field: 'lastPrice',       label: 'BLITZ/lastPrice' },

  // Tier 2: Worker pipelines
  { key: 20, symbol: 'NEXO', field: 'lastPrice', label: 'NEXO/Worker(mean)',
    pipeline: [{ type: 'Worker', fn: 'mean' }] },
  { key: 21, symbol: 'NEXO', field: 'lastPrice', label: 'NEXO/Worker(max)',
    pipeline: [{ type: 'Worker', fn: 'max' }] },
  { key: 22, symbol: 'NEXO', field: 'lastPrice', label: 'NEXO/Worker(spread)',
    pipeline: [{ type: 'Worker', fn: 'spread' }] },
  { key: 23, symbol: 'NEXO', field: 'lastPrice', label: 'NEXO/Worker(last→scale)',
    pipeline: [{ type: 'Worker', fn: 'last' }, { type: 'Worker', fn: 'scale', factor: 100 }] },

  // Tier 3: AtomicAccessor
  { key: 30, symbol: 'NEXO', field: 'lastPrice', label: 'NEXO/Atomic(rsi_14)',
    pipeline: [{ type: 'AtomicAccessor', field: 'rsi_14' }] },
  { key: 31, symbol: 'NEXO', field: 'lastPrice', label: 'NEXO/Atomic(ob.spread)',
    pipeline: [{ type: 'AtomicAccessor', field: 'ob.spread' }] },
  { key: 32, symbol: 'NEXO', field: 'lastPrice', label: 'NEXO/Atomic(ob.level.bid.1.price)',
    pipeline: [{ type: 'AtomicAccessor', field: 'ob.level.bid.1.price' }] },

  // Tier 4: Interval polling
  { key: 40, symbol: 'NEXO', field: 'lastPrice', label: 'NEXO/Interval(sma_5)',
    node: { type: 'Interval', ms: 1000,
            child: { type: 'AtomicAccessor', symbol: 'NEXO', field: 'sma_5' } } },
  { key: 41, symbol: 'NEXO', field: 'lastPrice', label: 'NEXO/Interval(ob.spread)',
    node: { type: 'Interval', ms: 500,
            child: { type: 'AtomicAccessor', symbol: 'NEXO', field: 'ob.spread' } } },

  // Tier 5: Multi-symbol
  { key: 50, symbol: 'QBIT', field: 'lastPrice', label: 'QBIT/Worker(mean)',
    pipeline: [{ type: 'Worker', fn: 'mean' }] },
  { key: 51, symbol: 'FLUX', field: 'lastPrice', label: 'FLUX/Atomic(macd_line)',
    pipeline: [{ type: 'AtomicAccessor', field: 'macd_line' }] },
  { key: 52, symbol: 'BLITZ', field: 'lastPrice', label: 'BLITZ/Worker(max)',
    pipeline: [{ type: 'Worker', fn: 'max' }] },

  // Tier 6: Aggregate batching
  { key: 60, symbol: 'NEXO', field: 'lastPrice', label: 'NEXO/Aggregate(5)',
    node: { type: 'Aggregate', arity: 5,
            inputs: [{ type: 'Listener', symbol: 'NEXO', field: 'lastPrice' }] } },
];

// ---------------------------------------------------------------------------
// State tracking
// ---------------------------------------------------------------------------
const stats = new Map();
for (const r of REQUESTS) {
  stats.set(r.key, {
    label:       r.label,
    count:       0,
    firstAt:     null,    // seconds since subscribe
    lastValue:   null,
  });
}

let subscribeTime = null;
let subscribeAcks = 0;
let errors        = [];

// ---------------------------------------------------------------------------
// Main
// ---------------------------------------------------------------------------
function log(msg) {
  console.log(`[smoke] ${msg}`);
}

function run() {
  log(`Connecting to ${URL} ...`);
  const ws = new WebSocket(URL);

  ws.on('error', (err) => {
    log(`WebSocket error: ${err.message}`);
    process.exit(1);
  });

  ws.on('open', () => {
    log(`Connected to ${URL}`);

    // Build subscribe payload — strip the 'label' field (not part of protocol)
    const requests = REQUESTS.map(({ label, ...rest }) => rest);
    const msg = JSON.stringify({ type: 'subscribe', requests });
    ws.send(msg);

    subscribeTime = Date.now();
    log(`Sent ${REQUESTS.length} subscribe requests`);
  });

  ws.on('message', (data) => {
    let msg;
    try {
      msg = JSON.parse(data.toString());
    } catch {
      log(`Unparseable message: ${data}`);
      return;
    }

    if (msg.type === 'subscribed') {
      subscribeAcks++;
      if (VERBOSE) log(`  ack key=${msg.key}`);
      return;
    }

    if (msg.type === 'error') {
      const detail = `${msg.where}: ${msg.message}`;
      errors.push(detail);
      log(`  ERROR ${detail}`);
      return;
    }

    if (msg.type === 'canceled') {
      if (VERBOSE) log(`  canceled key=${msg.key}`);
      return;
    }

    if (msg.type === 'update') {
      const s = stats.get(msg.key);
      if (!s) {
        if (VERBOSE) log(`  update for unknown key=${msg.key}`);
        return;
      }
      s.count++;
      if (s.firstAt === null) {
        s.firstAt = ((Date.now() - subscribeTime) / 1000).toFixed(1);
      }
      s.lastValue = msg.value;

      if (VERBOSE) {
        const valStr = typeof msg.value === 'object'
          ? JSON.stringify(msg.value)
          : String(msg.value);
        log(`  key=${String(msg.key).padEnd(3)} ${msg.symbol.padEnd(6)} `
          + `${s.label.padEnd(32)} = ${valStr}`);
      }
    }
  });

  ws.on('close', () => {
    log('Connection closed by server');
    report();
  });

  // Schedule shutdown after DURATION seconds
  setTimeout(() => {
    log(`\nDuration elapsed (${DURATION}s). Sending cancel...`);

    // Cancel all keys
    const keys = REQUESTS.map(r => r.key);
    ws.send(JSON.stringify({ type: 'cancel', keys }));

    // Give the server a moment to ack cancels, then close
    setTimeout(() => {
      ws.close();
      report();
    }, 1000);
  }, DURATION * 1000);
}

// ---------------------------------------------------------------------------
// Report
// ---------------------------------------------------------------------------
function report() {
  // Prevent double-report
  if (report._done) return;
  report._done = true;

  console.log('');
  log('=== RESULTS ===');
  console.log('');

  const keyWidth   = 5;
  const labelWidth = 34;

  // Header
  console.log(
    '  ' +
    'Key'.padEnd(keyWidth) + '  ' +
    'Subscription'.padEnd(labelWidth) + '  ' +
    'Updates'.padStart(8) + '  ' +
    'First (s)'.padStart(10)
  );
  console.log('  ' + '-'.repeat(keyWidth + 2 + labelWidth + 2 + 8 + 2 + 10));

  let totalUpdates = 0;
  let keysWithData = 0;
  let keysWithout  = [];

  for (const [key, s] of stats) {
    totalUpdates += s.count;
    if (s.count > 0) keysWithData++;
    else keysWithout.push(key);

    const firstStr = s.firstAt !== null ? `${s.firstAt}` : '-';
    console.log(
      '  ' +
      String(key).padEnd(keyWidth) + '  ' +
      s.label.padEnd(labelWidth) + '  ' +
      String(s.count).padStart(8) + '  ' +
      firstStr.padStart(10)
    );
  }

  console.log('');
  log('=== SUMMARY ===');
  log(`Duration: ${DURATION}s | Total updates: ${totalUpdates} | Keys with data: ${keysWithData}/${stats.size}`);
  log(`Subscribe acks: ${subscribeAcks}/${REQUESTS.length}`);

  if (errors.length > 0) {
    log(`Errors received: ${errors.length}`);
    for (const e of errors) log(`  - ${e}`);
  }

  if (keysWithout.length > 0) {
    log(`Keys with NO updates: ${keysWithout.join(', ')}`);
    log('FAIL');
    process.exit(1);
  } else {
    log('PASS');
    process.exit(0);
  }
}

run();
