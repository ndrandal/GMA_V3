'use strict';

// 7 deterministic test scenarios
// Each returns an array of NDJSON-serializable message objects

function singleSymbolLinear() {
  // 10 ticks of linear price movement for AAPL
  const msgs = [];
  for (let i = 0; i < 10; i++) {
    msgs.push({
      symbol: 'AAPL',
      lastPrice: +(187.50 + i * 0.05).toFixed(2),
      volume: 100 + i * 10,
      bid: +(187.49 + i * 0.05).toFixed(2),
      ask: +(187.51 + i * 0.05).toFixed(2),
    });
  }
  return { name: 'singleSymbolLinear', description: 'Basic tick ingestion, TA fires', msgs };
}

function obLifecycle() {
  // Full OB CRUD cycle: ticksize, adds, update, trade, delete
  const msgs = [
    { type: 'ob', action: 'ticksize', symbol: 'AAPL', tickSize: 0.01 },
    // Add bid orders
    { type: 'ob', action: 'add', symbol: 'AAPL', id: 1001, side: 'bid', price: 187.40, size: 200, priority: 1 },
    { type: 'ob', action: 'add', symbol: 'AAPL', id: 1002, side: 'bid', price: 187.39, size: 300, priority: 1 },
    { type: 'ob', action: 'add', symbol: 'AAPL', id: 1003, side: 'bid', price: 187.38, size: 150, priority: 1 },
    // Add ask orders
    { type: 'ob', action: 'add', symbol: 'AAPL', id: 2001, side: 'ask', price: 187.42, size: 250, priority: 1 },
    { type: 'ob', action: 'add', symbol: 'AAPL', id: 2002, side: 'ask', price: 187.43, size: 180, priority: 1 },
    { type: 'ob', action: 'add', symbol: 'AAPL', id: 2003, side: 'ask', price: 187.44, size: 400, priority: 1 },
    // Update a bid
    { type: 'ob', action: 'update', symbol: 'AAPL', id: 1001, size: 150 },
    // Trade
    { type: 'ob', action: 'trade', symbol: 'AAPL', price: 187.42, size: 100, aggressor: 'buy' },
    // Delete the traded ask
    { type: 'ob', action: 'delete', symbol: 'AAPL', id: 2001 },
    // Add replacement
    { type: 'ob', action: 'add', symbol: 'AAPL', id: 2004, side: 'ask', price: 187.42, size: 150, priority: 2 },
    // Another trade
    { type: 'ob', action: 'trade', symbol: 'AAPL', price: 187.40, size: 50, aggressor: 'sell' },
    // Delete some bids
    { type: 'ob', action: 'delete', symbol: 'AAPL', id: 1002 },
    { type: 'ob', action: 'delete', symbol: 'AAPL', id: 1003 },
  ];
  return { name: 'obLifecycle', description: 'OB CRUD: add/update/trade/delete', msgs };
}

function multiSymbolInterleaved() {
  // 3 symbols interleaved — tests per-symbol isolation
  const symbols = ['AAPL', 'MSFT', 'GOOGL'];
  const bases = [187.50, 415.20, 141.80];
  const msgs = [];
  for (let i = 0; i < 5; i++) {
    for (let s = 0; s < 3; s++) {
      msgs.push({
        symbol: symbols[s],
        lastPrice: +(bases[s] + i * 0.10).toFixed(2),
        volume: 200 + i * 50,
        bid: +(bases[s] + i * 0.10 - 0.01).toFixed(2),
        ask: +(bases[s] + i * 0.10 + 0.01).toFixed(2),
      });
    }
  }
  return { name: 'multiSymbolInterleaved', description: 'Per-symbol isolation', msgs };
}

function burst() {
  // 1000 rapid ticks for NVDA — stress test
  const msgs = [];
  let price = 875.30;
  for (let i = 0; i < 1000; i++) {
    price += (Math.sin(i * 0.1) * 0.5);
    price = Math.max(1, price);
    msgs.push({
      symbol: 'NVDA',
      lastPrice: +price.toFixed(2),
      volume: 100,
      bid: +(price - 0.01).toFixed(2),
      ask: +(price + 0.01).toFixed(2),
    });
  }
  return { name: 'burst', description: 'Stress: 1000 rapid ticks', msgs };
}

function mixedFeed() {
  // Interleaved tick + OB messages for MSFT
  const msgs = [];
  const base = 415.20;

  // Set up OB
  msgs.push({ type: 'ob', action: 'ticksize', symbol: 'MSFT', tickSize: 0.01 });

  for (let i = 0; i < 10; i++) {
    const px = +(base + i * 0.03).toFixed(2);
    // Market tick
    msgs.push({
      symbol: 'MSFT',
      lastPrice: px,
      volume: 150 + i * 20,
      bid: +(px - 0.02).toFixed(2),
      ask: +(px + 0.02).toFixed(2),
    });
    // OB add on bid side
    msgs.push({
      type: 'ob', action: 'add', symbol: 'MSFT',
      id: 3000 + i * 2, side: 'bid', price: +(px - 0.02).toFixed(2),
      size: 100 + i * 30, priority: i + 1,
    });
    // OB add on ask side
    msgs.push({
      type: 'ob', action: 'add', symbol: 'MSFT',
      id: 3001 + i * 2, side: 'ask', price: +(px + 0.02).toFixed(2),
      size: 100 + i * 25, priority: i + 1,
    });
    // Occasional trade
    if (i % 3 === 0) {
      msgs.push({
        type: 'ob', action: 'trade', symbol: 'MSFT',
        price: px, size: 50, aggressor: i % 2 === 0 ? 'buy' : 'sell',
      });
    }
  }

  return { name: 'mixedFeed', description: 'Interleaved tick + OB', msgs };
}

function volumePattern() {
  // Simulates intraday volume curve (U-shaped) for SPY — 78 ticks over ~6.5h
  const msgs = [];
  const minutesInDay = 390; // 6.5 hours
  const intervals = 78;     // 5-minute buckets
  let price = 502.30;

  for (let i = 0; i < intervals; i++) {
    const t = i / intervals; // 0..1 through the day
    // U-shaped volume: high at open, low midday, high at close
    const volumeMultiplier = 2.5 - 4.0 * t * (1 - t);
    const volume = Math.round(200 * Math.max(0.3, volumeMultiplier));

    // Small random walk
    price += (Math.sin(i * 0.2) * 0.05 + (i < intervals / 2 ? 0.02 : -0.01));
    price = Math.max(1, price);

    msgs.push({
      symbol: 'SPY',
      lastPrice: +price.toFixed(2),
      volume,
      bid: +(price - 0.01).toFixed(2),
      ask: +(price + 0.01).toFixed(2),
    });
  }

  return { name: 'volumePattern', description: 'Intraday volume curve', msgs };
}

function obGapRecovery() {
  // Feed reset + rebuild scenario
  const msgs = [
    // Initial state
    { type: 'ob', action: 'ticksize', symbol: 'GOOGL', tickSize: 0.01 },
    { type: 'ob', action: 'add', symbol: 'GOOGL', id: 5001, side: 'bid', price: 141.79, size: 300, priority: 1 },
    { type: 'ob', action: 'add', symbol: 'GOOGL', id: 5002, side: 'ask', price: 141.81, size: 200, priority: 1 },
    // Simulate gap — send reset
    { type: 'control', action: 'reset', symbol: 'GOOGL', epoch: 2 },
    // Rebuild after reset
    { type: 'ob', action: 'ticksize', symbol: 'GOOGL', tickSize: 0.01 },
    { type: 'ob', action: 'add', symbol: 'GOOGL', id: 6001, side: 'bid', price: 141.75, size: 500, priority: 1 },
    { type: 'ob', action: 'add', symbol: 'GOOGL', id: 6002, side: 'ask', price: 141.77, size: 350, priority: 1 },
    { type: 'ob', action: 'trade', symbol: 'GOOGL', price: 141.77, size: 100, aggressor: 'buy' },
  ];
  return { name: 'obGapRecovery', description: 'Feed reset + rebuild', msgs };
}

const ALL_SCENARIOS = {
  singleSymbolLinear,
  obLifecycle,
  multiSymbolInterleaved,
  burst,
  mixedFeed,
  volumePattern,
  obGapRecovery,
};

module.exports = { ALL_SCENARIOS };
