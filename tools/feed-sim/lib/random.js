'use strict';

// Seedable PRNG — mulberry32
function mulberry32(seed) {
  let s = seed | 0;
  return function () {
    s = (s + 0x6d2b79f5) | 0;
    let t = Math.imul(s ^ (s >>> 15), 1 | s);
    t = (t + Math.imul(t ^ (t >>> 7), 61 | t)) ^ t;
    return ((t ^ (t >>> 14)) >>> 0) / 4294967296;
  };
}

// Box-Muller transform for gaussian samples
function gaussianPair(rng) {
  let u, v, s;
  do {
    u = 2 * rng() - 1;
    v = 2 * rng() - 1;
    s = u * u + v * v;
  } while (s >= 1 || s === 0);
  const f = Math.sqrt((-2 * Math.log(s)) / s);
  return [u * f, v * f];
}

class Random {
  constructor(seed) {
    this._rng = mulberry32(seed);
    this._spare = null;
  }

  // Uniform [0, 1)
  next() {
    return this._rng();
  }

  // Uniform integer in [min, max]
  int(min, max) {
    return min + Math.floor(this._rng() * (max - min + 1));
  }

  // Gaussian (mean, stddev)
  gaussian(mean = 0, std = 1) {
    if (this._spare !== null) {
      const v = this._spare;
      this._spare = null;
      return mean + v * std;
    }
    const [a, b] = gaussianPair(this._rng);
    this._spare = b;
    return mean + a * std;
  }

  // Pick a random element from an array
  pick(arr) {
    return arr[Math.floor(this._rng() * arr.length)];
  }

  // Weighted pick: weights[i] corresponds to items[i]
  weightedPick(items, weights) {
    const total = weights.reduce((a, b) => a + b, 0);
    let r = this._rng() * total;
    for (let i = 0; i < items.length; i++) {
      r -= weights[i];
      if (r <= 0) return items[i];
    }
    return items[items.length - 1];
  }

  // Bernoulli trial
  chance(p) {
    return this._rng() < p;
  }
}

module.exports = { Random };
