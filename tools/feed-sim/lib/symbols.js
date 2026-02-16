'use strict';

// 30-symbol universe across 7 sectors
// Each entry: { symbol, name, sector, basePrice, tickSize, volatility }
// volatility is a multiplier on the base daily vol (~0.02)

const UNIVERSE = [
  // Tech (6)
  { symbol: 'AAPL',  sector: 'tech',       basePrice: 187.50, tickSize: 0.01, volatility: 1.0 },
  { symbol: 'MSFT',  sector: 'tech',       basePrice: 415.20, tickSize: 0.01, volatility: 0.9 },
  { symbol: 'GOOGL', sector: 'tech',       basePrice: 141.80, tickSize: 0.01, volatility: 1.1 },
  { symbol: 'NVDA',  sector: 'tech',       basePrice: 875.30, tickSize: 0.01, volatility: 2.0 },
  { symbol: 'TSLA',  sector: 'tech',       basePrice: 245.60, tickSize: 0.01, volatility: 2.5 },
  { symbol: 'META',  sector: 'tech',       basePrice: 485.90, tickSize: 0.01, volatility: 1.3 },

  // Finance (5)
  { symbol: 'JPM',   sector: 'finance',    basePrice: 195.40, tickSize: 0.01, volatility: 0.8 },
  { symbol: 'BAC',   sector: 'finance',    basePrice:  35.20, tickSize: 0.01, volatility: 0.9 },
  { symbol: 'GS',    sector: 'finance',    basePrice: 385.70, tickSize: 0.01, volatility: 1.0 },
  { symbol: 'MS',    sector: 'finance',    basePrice:  87.30, tickSize: 0.01, volatility: 0.9 },
  { symbol: 'V',     sector: 'finance',    basePrice: 275.10, tickSize: 0.01, volatility: 0.7 },

  // Healthcare (4)
  { symbol: 'JNJ',   sector: 'healthcare', basePrice: 155.80, tickSize: 0.01, volatility: 0.5 },
  { symbol: 'PFE',   sector: 'healthcare', basePrice:  28.40, tickSize: 0.01, volatility: 0.8 },
  { symbol: 'UNH',   sector: 'healthcare', basePrice: 525.60, tickSize: 0.01, volatility: 0.7 },
  { symbol: 'ABBV',  sector: 'healthcare', basePrice: 162.30, tickSize: 0.01, volatility: 0.6 },

  // Energy (4)
  { symbol: 'XOM',   sector: 'energy',     basePrice: 105.20, tickSize: 0.01, volatility: 0.9 },
  { symbol: 'CVX',   sector: 'energy',     basePrice: 155.80, tickSize: 0.01, volatility: 0.8 },
  { symbol: 'COP',   sector: 'energy',     basePrice: 115.40, tickSize: 0.01, volatility: 1.0 },
  { symbol: 'SLB',   sector: 'energy',     basePrice:  52.30, tickSize: 0.01, volatility: 1.1 },

  // Consumer (4)
  { symbol: 'AMZN',  sector: 'consumer',   basePrice: 178.50, tickSize: 0.01, volatility: 1.2 },
  { symbol: 'KO',    sector: 'consumer',   basePrice:  59.80, tickSize: 0.01, volatility: 0.4 },
  { symbol: 'PG',    sector: 'consumer',   basePrice: 162.40, tickSize: 0.01, volatility: 0.4 },
  { symbol: 'WMT',   sector: 'consumer',   basePrice: 165.30, tickSize: 0.01, volatility: 0.5 },

  // Industrial (4)
  { symbol: 'CAT',   sector: 'industrial', basePrice: 295.70, tickSize: 0.01, volatility: 0.9 },
  { symbol: 'BA',    sector: 'industrial', basePrice: 215.40, tickSize: 0.01, volatility: 1.5 },
  { symbol: 'GE',    sector: 'industrial', basePrice: 155.90, tickSize: 0.01, volatility: 1.0 },
  { symbol: 'HON',   sector: 'industrial', basePrice: 205.20, tickSize: 0.01, volatility: 0.7 },

  // ETFs (3)
  { symbol: 'SPY',   sector: 'etf',        basePrice: 502.30, tickSize: 0.01, volatility: 0.6 },
  { symbol: 'QQQ',   sector: 'etf',        basePrice: 435.10, tickSize: 0.01, volatility: 0.8 },
  { symbol: 'IWM',   sector: 'etf',        basePrice: 202.50, tickSize: 0.01, volatility: 0.9 },
];

const SECTORS = [...new Set(UNIVERSE.map(s => s.sector))];

function getUniverse(count) {
  return UNIVERSE.slice(0, Math.min(count, UNIVERSE.length));
}

module.exports = { UNIVERSE, SECTORS, getUniverse };
