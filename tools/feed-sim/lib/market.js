'use strict';

// Geometric Brownian Motion price engine with correlated sector moves

class MarketEngine {
  constructor(symbols, rng) {
    this.rng = rng;
    this.symbols = symbols;
    this.state = new Map();

    // Initialize per-symbol state
    for (const sym of symbols) {
      this.state.set(sym.symbol, {
        price: sym.basePrice,
        tickSize: sym.tickSize,
        volatility: sym.volatility,
        sector: sym.sector,
        volume: 0,
      });
    }

    // Base params
    this.baseDailyVol = 0.02;    // ~2% daily volatility
    this.sectorCorrelation = 0.6; // intra-sector correlation
    this.ticksPerDay = 23400;     // 6.5 hours * 3600
    this.spikeProb = 0.02;        // 2% chance of volume spike
    this.baseVolume = 100;
  }

  // Advance one tick for all symbols, returns array of tick messages
  tick() {
    // Generate one sector shock per sector
    const sectorShocks = new Map();
    const sectors = [...new Set(this.symbols.map(s => s.sector))];
    for (const sec of sectors) {
      sectorShocks.set(sec, this.rng.gaussian(0, 1));
    }

    const ticks = [];

    for (const sym of this.symbols) {
      const st = this.state.get(sym.symbol);
      const perTickVol = (this.baseDailyVol * st.volatility) / Math.sqrt(this.ticksPerDay);

      // Correlated return: sector component + idiosyncratic component
      const sectorZ = sectorShocks.get(st.sector);
      const idioZ = this.rng.gaussian(0, 1);
      const z = this.sectorCorrelation * sectorZ +
                Math.sqrt(1 - this.sectorCorrelation * this.sectorCorrelation) * idioZ;

      // GBM step: S(t+1) = S(t) * exp((mu - sigma^2/2)*dt + sigma*sqrt(dt)*Z)
      const drift = -0.5 * perTickVol * perTickVol; // risk-neutral
      const logReturn = drift + perTickVol * z;
      let newPrice = st.price * Math.exp(logReturn);

      // Snap to tick size
      newPrice = Math.round(newPrice / st.tickSize) * st.tickSize;
      newPrice = Math.max(st.tickSize, newPrice); // floor at one tick

      st.price = newPrice;

      // Volume
      let vol = Math.round(this.baseVolume * (0.5 + this.rng.next()));
      if (this.rng.chance(this.spikeProb)) {
        vol = Math.round(vol * (3 + this.rng.next() * 7)); // 3x-10x spike
      }
      st.volume += vol;

      // Build spread
      const halfSpread = st.tickSize * (1 + Math.floor(this.rng.next() * 3));
      const bid = Math.round((newPrice - halfSpread) / st.tickSize) * st.tickSize;
      const ask = Math.round((newPrice + halfSpread) / st.tickSize) * st.tickSize;

      ticks.push({
        symbol: sym.symbol,
        lastPrice: +newPrice.toFixed(4),
        volume: vol,
        bid: +Math.max(st.tickSize, bid).toFixed(4),
        ask: +ask.toFixed(4),
      });
    }

    return ticks;
  }

  // Get current price for a symbol
  getPrice(symbol) {
    const st = this.state.get(symbol);
    return st ? st.price : 0;
  }
}

module.exports = { MarketEngine };
