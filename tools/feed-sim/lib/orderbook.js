'use strict';

// Order book depth simulation — generates OB messages for a symbol

class OrderBookSim {
  constructor(symbol, tickSize, rng) {
    this.symbol = symbol;
    this.tickSize = tickSize;
    this.rng = rng;
    this.nextId = 1;
    this.orders = new Map(); // id -> { side, price, size }
    this.initialized = false;
  }

  // Initialize the book around a mid price
  // Returns array of OB messages (ticksize + adds)
  init(midPrice, levels = 10, ordersPerLevel = 3) {
    this.orders.clear();
    this.nextId = 1;
    this.initialized = true;

    const msgs = [];

    // Send ticksize message
    msgs.push({
      type: 'ob',
      action: 'ticksize',
      symbol: this.symbol,
      tickSize: this.tickSize,
    });

    // Build bid levels (below mid)
    for (let l = 1; l <= levels; l++) {
      const price = +(midPrice - l * this.tickSize).toFixed(4);
      if (price <= 0) continue;
      for (let o = 0; o < ordersPerLevel; o++) {
        const id = this.nextId++;
        const size = 50 + Math.floor(this.rng.next() * 450); // 50-499
        this.orders.set(id, { side: 'bid', price, size });
        msgs.push({
          type: 'ob', action: 'add', symbol: this.symbol,
          id, side: 'bid', price, size, priority: o + 1,
        });
      }
    }

    // Build ask levels (above mid)
    for (let l = 1; l <= levels; l++) {
      const price = +(midPrice + l * this.tickSize).toFixed(4);
      for (let o = 0; o < ordersPerLevel; o++) {
        const id = this.nextId++;
        const size = 50 + Math.floor(this.rng.next() * 450);
        this.orders.set(id, { side: 'ask', price, size });
        msgs.push({
          type: 'ob', action: 'add', symbol: this.symbol,
          id, side: 'ask', price, size, priority: o + 1,
        });
      }
    }

    return msgs;
  }

  // Generate one cycle of OB activity around the current mid price
  // Action distribution: 30% adds, 20% cancels, 15% updates, 15% trades, 20% replenishment
  cycle(midPrice) {
    if (!this.initialized) return this.init(midPrice);

    const msgs = [];
    const actions = this.rng.weightedPick(
      ['add', 'cancel', 'update', 'trade', 'replenish'],
      [30, 20, 15, 15, 20]
    );

    switch (actions) {
      case 'add':
        msgs.push(...this._doAdd(midPrice));
        break;
      case 'cancel':
        msgs.push(...this._doCancel());
        break;
      case 'update':
        msgs.push(...this._doUpdate());
        break;
      case 'trade':
        msgs.push(...this._doTrade(midPrice));
        break;
      case 'replenish':
        msgs.push(...this._doReplenish(midPrice));
        break;
    }

    return msgs;
  }

  _doAdd(midPrice) {
    const side = this.rng.chance(0.5) ? 'bid' : 'ask';
    const offset = (1 + Math.floor(this.rng.next() * 10)) * this.tickSize;
    let price = side === 'bid'
      ? midPrice - offset
      : midPrice + offset;
    price = +Math.max(this.tickSize, Math.round(price / this.tickSize) * this.tickSize).toFixed(4);

    const id = this.nextId++;
    const size = 50 + Math.floor(this.rng.next() * 450);
    this.orders.set(id, { side, price, size });

    return [{
      type: 'ob', action: 'add', symbol: this.symbol,
      id, side, price, size, priority: Math.floor(this.rng.next() * 100),
    }];
  }

  _doCancel() {
    const ids = [...this.orders.keys()];
    if (ids.length === 0) return [];
    const id = this.rng.pick(ids);
    this.orders.delete(id);
    return [{
      type: 'ob', action: 'delete', symbol: this.symbol, id,
    }];
  }

  _doUpdate() {
    const ids = [...this.orders.keys()];
    if (ids.length === 0) return [];
    const id = this.rng.pick(ids);
    const order = this.orders.get(id);
    const newSize = Math.max(1, order.size + Math.floor(this.rng.gaussian(0, 50)));
    order.size = newSize;
    return [{
      type: 'ob', action: 'update', symbol: this.symbol,
      id, size: newSize,
    }];
  }

  _doTrade(midPrice) {
    const aggressor = this.rng.chance(0.5) ? 'buy' : 'sell';
    const price = +midPrice.toFixed(4);
    const size = 10 + Math.floor(this.rng.next() * 200);
    return [{
      type: 'ob', action: 'trade', symbol: this.symbol,
      price, size, aggressor,
    }];
  }

  _doReplenish(midPrice) {
    // Add orders to thin levels
    const msgs = [];
    const side = this.rng.chance(0.5) ? 'bid' : 'ask';
    const count = 1 + Math.floor(this.rng.next() * 3);
    for (let i = 0; i < count; i++) {
      const offset = (1 + Math.floor(this.rng.next() * 5)) * this.tickSize;
      let price = side === 'bid'
        ? midPrice - offset
        : midPrice + offset;
      price = +Math.max(this.tickSize, Math.round(price / this.tickSize) * this.tickSize).toFixed(4);

      const id = this.nextId++;
      const size = 100 + Math.floor(this.rng.next() * 300);
      this.orders.set(id, { side, price, size });
      msgs.push({
        type: 'ob', action: 'add', symbol: this.symbol,
        id, side, price, size, priority: Math.floor(this.rng.next() * 100),
      });
    }
    return msgs;
  }
}

module.exports = { OrderBookSim };
