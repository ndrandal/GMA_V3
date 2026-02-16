#!/usr/bin/env node
'use strict';

const { Random } = require('./lib/random');
const { getUniverse } = require('./lib/symbols');
const { Connection } = require('./lib/connection');
const { MarketEngine } = require('./lib/market');
const { OrderBookSim } = require('./lib/orderbook');
const { ALL_SCENARIOS } = require('./lib/scenarios');

// ---- CLI argument parsing ----
function parseArgs(argv) {
  const args = {
    mode: 'demo',
    host: '127.0.0.1',
    port: 9001,
    symbols: 30,
    seed: null,      // null = mode-dependent default
    tickMs: 100,
    duration: 0,     // 0 = indefinite
    scenario: null,
    noOb: false,
    verbose: false,
  };

  for (let i = 2; i < argv.length; i++) {
    const a = argv[i];
    switch (a) {
      case '--demo': case '-d':
        args.mode = 'demo'; break;
      case '--test': case '-t':
        args.mode = 'test'; break;
      case '--host':
        args.host = argv[++i]; break;
      case '--port': case '-p':
        args.port = parseInt(argv[++i], 10); break;
      case '--symbols': case '-n':
        args.symbols = parseInt(argv[++i], 10); break;
      case '--seed': case '-s':
        args.seed = parseInt(argv[++i], 10); break;
      case '--tick-ms':
        args.tickMs = parseInt(argv[++i], 10); break;
      case '--duration':
        args.duration = parseInt(argv[++i], 10); break;
      case '--scenario':
        args.scenario = argv[++i]; break;
      case '--no-ob':
        args.noOb = true; break;
      case '--verbose': case '-v':
        args.verbose = true; break;
      case '--help': case '-h':
        printUsage(); process.exit(0);
      default:
        console.error(`Unknown option: ${a}`);
        printUsage(); process.exit(1);
    }
  }

  // Mode-dependent seed defaults
  if (args.seed === null) {
    args.seed = args.mode === 'test' ? 42 : Date.now();
  }

  return args;
}

function printUsage() {
  console.log(`
Usage: node tools/feed-sim/index.js [options]

Modes:
  --demo, -d         Continuous realistic feed (default)
  --test, -t         Deterministic test scenarios

Options:
  --host <host>      Default: 127.0.0.1
  --port, -p <port>  Default: 9001
  --symbols, -n <N>  Default: 30
  --seed, -s <seed>  Default: Date.now (demo), 42 (test)
  --tick-ms <ms>     Tick interval, default: 100
  --duration <sec>   0 = indefinite (default)
  --scenario <name>  Run single scenario (test mode)
  --no-ob            Disable order book messages
  --verbose, -v      Echo messages to stdout
  --help, -h         Show this help
`);
}

// ---- Demo mode ----
async function runDemo(args) {
  const rng = new Random(args.seed);
  const symbols = getUniverse(args.symbols);
  const conn = new Connection(args.host, args.port, { verbose: args.verbose });

  console.log(`[feed-sim] demo mode | seed=${args.seed} symbols=${symbols.length} tick=${args.tickMs}ms`);
  console.log(`[feed-sim] connecting to ${args.host}:${args.port}...`);
  await conn.connect();
  console.log('[feed-sim] connected');

  const engine = new MarketEngine(symbols, rng);

  // Initialize OB simulators
  const obSims = new Map();
  if (!args.noOb) {
    for (const sym of symbols) {
      obSims.set(sym.symbol, new OrderBookSim(sym.symbol, sym.tickSize, rng));
    }
  }

  const startTime = Date.now();
  let tickCount = 0;
  let obMsgCount = 0;

  // Graceful shutdown
  let running = true;
  process.on('SIGINT', () => { running = false; });
  process.on('SIGTERM', () => { running = false; });

  while (running) {
    // Check duration
    if (args.duration > 0 && (Date.now() - startTime) / 1000 >= args.duration) {
      break;
    }

    // Generate market ticks
    const ticks = engine.tick();
    conn.sendBatch(ticks);
    tickCount += ticks.length;

    // Generate OB activity
    if (!args.noOb) {
      for (const sym of symbols) {
        const sim = obSims.get(sym.symbol);
        const mid = engine.getPrice(sym.symbol);
        const obMsgs = sim.cycle(mid);
        if (obMsgs.length > 0) {
          conn.sendBatch(obMsgs);
          obMsgCount += obMsgs.length;
        }
      }
    }

    // Wait for next tick
    await sleep(args.tickMs);
  }

  const elapsed = ((Date.now() - startTime) / 1000).toFixed(1);
  console.log(`\n[feed-sim] done | ${elapsed}s | ticks=${tickCount} ob_msgs=${obMsgCount} total=${conn.messageCount}`);
  await conn.close();
}

// ---- Test mode ----
async function runTest(args) {
  const conn = new Connection(args.host, args.port, { verbose: args.verbose });

  console.log(`[feed-sim] test mode | seed=${args.seed}`);
  console.log(`[feed-sim] connecting to ${args.host}:${args.port}...`);
  await conn.connect();
  console.log('[feed-sim] connected');

  const scenarioNames = args.scenario
    ? [args.scenario]
    : Object.keys(ALL_SCENARIOS);

  let totalMsgs = 0;
  let passed = 0;

  for (const name of scenarioNames) {
    const factory = ALL_SCENARIOS[name];
    if (!factory) {
      console.error(`  [SKIP] unknown scenario: ${name}`);
      continue;
    }

    const { description, msgs } = factory();
    process.stdout.write(`  [${name}] ${description} (${msgs.length} msgs)... `);

    try {
      // Send with small delay between messages for ordering
      for (const msg of msgs) {
        conn.send(msg);
      }
      // Small settling pause between scenarios
      await sleep(50);

      totalMsgs += msgs.length;
      passed++;
      console.log('OK');
    } catch (err) {
      console.log(`FAIL: ${err.message}`);
    }
  }

  console.log(`\n[feed-sim] ${passed}/${scenarioNames.length} scenarios sent | ${totalMsgs} total messages`);
  await conn.close();
}

// ---- Helpers ----
function sleep(ms) {
  return new Promise(resolve => setTimeout(resolve, ms));
}

// ---- Main ----
async function main() {
  const args = parseArgs(process.argv);

  try {
    if (args.mode === 'test') {
      await runTest(args);
    } else {
      await runDemo(args);
    }
  } catch (err) {
    console.error(`[feed-sim] error: ${err.message}`);
    process.exit(1);
  }
}

main();
