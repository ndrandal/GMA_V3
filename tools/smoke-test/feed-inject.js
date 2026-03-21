#!/usr/bin/env node
// feed-inject.js — Minimal TCP feed injector for smoke testing
//
// Sends synthetic market ticks + OB messages to GMA's TCP feed port (9001).
// Generates ticks for all symbols used by smoke.js at ~10 ticks/sec.

'use strict';

const net = require('net');

const HOST = process.argv[2] || 'localhost';
const PORT = parseInt(process.argv[3] || '9001', 10);
const RATE = parseInt(process.argv[4] || '100', 10); // ms between ticks

const SYMBOLS = ['NEXO', 'VALT', 'BLITZ', 'QBIT', 'FLUX'];

// Simulated prices per symbol
const state = {};
for (const sym of SYMBOLS) {
  state[sym] = {
    price:  100 + Math.random() * 50,
    volume: Math.floor(1000 + Math.random() * 5000),
    orderId: 1000,
  };
}

function randomWalk(val, pct) {
  return val * (1 + (Math.random() - 0.5) * 2 * pct);
}

let tickCount = 0;
let obCount   = 0;

function log(msg) {
  console.log(`[feed-inject] ${msg}`);
}

const client = net.createConnection({ host: HOST, port: PORT }, () => {
  log(`Connected to ${HOST}:${PORT}`);
  log(`Sending ticks for: ${SYMBOLS.join(', ')} every ${RATE}ms`);

  const interval = setInterval(() => {
    for (const sym of SYMBOLS) {
      const s = state[sym];

      // Walk price & volume
      s.price  = randomWalk(s.price, 0.002);
      s.volume = Math.max(100, Math.floor(randomWalk(s.volume, 0.05)));

      // Market tick
      const tick = JSON.stringify({
        symbol:    sym,
        lastPrice: parseFloat(s.price.toFixed(4)),
        volume:    s.volume,
        bid:       parseFloat((s.price * 0.999).toFixed(4)),
        ask:       parseFloat((s.price * 1.001).toFixed(4)),
        high:      parseFloat((s.price * 1.01).toFixed(4)),
        low:       parseFloat((s.price * 0.99).toFixed(4)),
        open:      parseFloat((s.price * 0.998).toFixed(4)),
        timestamp: Date.now(),
      });
      client.write(tick + '\n');
      tickCount++;

      // OB messages — add bid & ask orders every tick
      s.orderId++;
      const bidOrder = JSON.stringify({
        type:   'ob',
        action: 'add',
        symbol: sym,
        id:     s.orderId * 2,
        side:   'bid',
        price:  parseFloat((s.price * 0.999).toFixed(4)),
        size:   Math.floor(100 + Math.random() * 500),
      });
      client.write(bidOrder + '\n');
      obCount++;

      s.orderId++;
      const askOrder = JSON.stringify({
        type:   'ob',
        action: 'add',
        symbol: sym,
        id:     s.orderId * 2 + 1,
        side:   'ask',
        price:  parseFloat((s.price * 1.001).toFixed(4)),
        size:   Math.floor(100 + Math.random() * 500),
      });
      client.write(askOrder + '\n');
      obCount++;
    }
  }, RATE);

  // Status every 5 seconds
  const statusInterval = setInterval(() => {
    log(`Sent ${tickCount} ticks, ${obCount} OB messages`);
  }, 5000);

  // Cleanup on close
  client.on('close', () => {
    clearInterval(interval);
    clearInterval(statusInterval);
    log(`Disconnected. Total: ${tickCount} ticks, ${obCount} OB messages`);
  });
});

client.on('error', (err) => {
  log(`Connection error: ${err.message}`);
  process.exit(1);
});

// Graceful shutdown
process.on('SIGINT', () => {
  log('Shutting down...');
  client.end();
});
process.on('SIGTERM', () => {
  log('Shutting down...');
  client.end();
});
