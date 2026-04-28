"""
Few-shot examples for NL → TreeBuilder JSON, ordered by ascending complexity.

Each example is a (natural_language, expected_json) pair.
Covers all 7 node types and key composition patterns.
"""

EXAMPLES = [
    # ------------------------------------------------------------------
    # TIER 1: Simple single-node subscriptions
    # ------------------------------------------------------------------
    {
        "nl": "Stream AAPL's price",
        "json": {
            "type": "subscribe",
            "requests": [
                {
                    "key": 1,
                    "symbol": "AAPL",
                    "field": "lastPrice"
                }
            ]
        }
    },
    {
        "nl": "Show me Tesla's volume in real time",
        "json": {
            "type": "subscribe",
            "requests": [
                {
                    "key": 1,
                    "symbol": "TSLA",
                    "field": "volume"
                }
            ]
        }
    },
    {
        "nl": "Subscribe to Google's bid price",
        "json": {
            "type": "subscribe",
            "requests": [
                {
                    "key": 1,
                    "symbol": "GOOG",
                    "field": "bid"
                }
            ]
        }
    },

    # ------------------------------------------------------------------
    # TIER 2: Single Worker node
    # ------------------------------------------------------------------
    {
        "nl": "Give me the running average of AAPL's price",
        "json": {
            "type": "subscribe",
            "requests": [
                {
                    "key": 1,
                    "symbol": "AAPL",
                    "field": "lastPrice",
                    "node": {"type": "Worker", "fn": "mean"}
                }
            ]
        }
    },
    {
        "nl": "Track the cumulative sum of MSFT volume",
        "json": {
            "type": "subscribe",
            "requests": [
                {
                    "key": 1,
                    "symbol": "MSFT",
                    "field": "volume",
                    "node": {"type": "Worker", "fn": "sum"}
                }
            ]
        }
    },
    {
        "nl": "What's the max price NVDA has hit?",
        "json": {
            "type": "subscribe",
            "requests": [
                {
                    "key": 1,
                    "symbol": "NVDA",
                    "field": "lastPrice",
                    "node": {"type": "Worker", "fn": "max"}
                }
            ]
        }
    },
    {
        "nl": "Show me AAPL's price change since open, scaled to basis points",
        "json": {
            "type": "subscribe",
            "requests": [
                {
                    "key": 1,
                    "symbol": "AAPL",
                    "field": "lastPrice",
                    "pipeline": [
                        {"type": "Worker", "fn": "diff"},
                        {"type": "Worker", "fn": "scale", "factor": 10000.0}
                    ]
                }
            ]
        }
    },

    # ------------------------------------------------------------------
    # TIER 3: AtomicAccessor (TA indicators)
    # ------------------------------------------------------------------
    {
        "nl": "Stream AAPL's 20-period SMA",
        "json": {
            "type": "subscribe",
            "requests": [
                {
                    "key": 1,
                    "symbol": "AAPL",
                    "field": "lastPrice",
                    "node": {"type": "AtomicAccessor", "symbol": "AAPL", "field": "sma_20"}
                }
            ]
        }
    },
    {
        "nl": "Show me Tesla's RSI",
        "json": {
            "type": "subscribe",
            "requests": [
                {
                    "key": 1,
                    "symbol": "TSLA",
                    "field": "lastPrice",
                    "node": {"type": "AtomicAccessor", "symbol": "TSLA", "field": "rsi_14"}
                }
            ]
        }
    },
    {
        "nl": "What's the MACD histogram for GOOG?",
        "json": {
            "type": "subscribe",
            "requests": [
                {
                    "key": 1,
                    "symbol": "GOOG",
                    "field": "lastPrice",
                    "node": {"type": "AtomicAccessor", "symbol": "GOOG", "field": "macd_histogram"}
                }
            ]
        }
    },
    {
        "nl": "Give me AAPL's VWAP",
        "json": {
            "type": "subscribe",
            "requests": [
                {
                    "key": 1,
                    "symbol": "AAPL",
                    "field": "lastPrice",
                    "node": {"type": "AtomicAccessor", "symbol": "AAPL", "field": "vwap"}
                }
            ]
        }
    },

    # ------------------------------------------------------------------
    # TIER 4: Aggregate (cross-field, cross-symbol)
    # ------------------------------------------------------------------
    {
        "nl": "Show me AAPL's bid-ask spread",
        "json": {
            "type": "subscribe",
            "requests": [
                {
                    "key": 1,
                    "symbol": "AAPL",
                    "field": "lastPrice",
                    "node": {
                        "type": "Aggregate",
                        "arity": 2,
                        "inputs": [
                            {"type": "Listener", "symbol": "AAPL", "field": "ask"},
                            {"type": "Listener", "symbol": "AAPL", "field": "bid"}
                        ]
                    },
                    "pipeline": [{"type": "Worker", "fn": "diff"}]
                }
            ]
        }
    },
    {
        "nl": "Track the price difference between AAPL and MSFT",
        "json": {
            "type": "subscribe",
            "requests": [
                {
                    "key": 1,
                    "symbol": "AAPL",
                    "field": "lastPrice",
                    "node": {
                        "type": "Aggregate",
                        "arity": 2,
                        "inputs": [
                            {"type": "Listener", "symbol": "AAPL", "field": "lastPrice"},
                            {"type": "Listener", "symbol": "MSFT", "field": "lastPrice"}
                        ]
                    },
                    "pipeline": [{"type": "Worker", "fn": "diff"}]
                }
            ]
        }
    },

    # ------------------------------------------------------------------
    # TIER 5: Interval (periodic polling)
    # ------------------------------------------------------------------
    {
        "nl": "Poll AAPL's RSI every 2 seconds",
        "json": {
            "type": "subscribe",
            "requests": [
                {
                    "key": 1,
                    "symbol": "AAPL",
                    "field": "lastPrice",
                    "node": {
                        "type": "Interval",
                        "ms": 2000,
                        "child": {
                            "type": "AtomicAccessor",
                            "symbol": "AAPL",
                            "field": "rsi_14"
                        }
                    }
                }
            ]
        }
    },
    {
        "nl": "Every 10 seconds, give me MSFT's Bollinger upper band",
        "json": {
            "type": "subscribe",
            "requests": [
                {
                    "key": 1,
                    "symbol": "MSFT",
                    "field": "lastPrice",
                    "node": {
                        "type": "Interval",
                        "ms": 10000,
                        "child": {
                            "type": "AtomicAccessor",
                            "symbol": "MSFT",
                            "field": "bollinger_upper"
                        }
                    }
                }
            ]
        }
    },

    # ------------------------------------------------------------------
    # TIER 6: SymbolSplit
    # ------------------------------------------------------------------
    {
        "nl": "Track the running average price for every symbol in the feed",
        "json": {
            "type": "subscribe",
            "requests": [
                {
                    "key": 1,
                    "symbol": "*",
                    "field": "lastPrice",
                    "node": {
                        "type": "SymbolSplit",
                        "child": {"type": "Worker", "fn": "mean"}
                    }
                }
            ]
        }
    },

    # ------------------------------------------------------------------
    # TIER 7: Multi-leg / multi-request strategies
    # ------------------------------------------------------------------
    {
        "nl": "Monitor AAPL's SMA crossover: stream both the 20-period and 50-period SMA",
        "json": {
            "type": "subscribe",
            "requests": [
                {
                    "key": 1,
                    "symbol": "AAPL",
                    "field": "lastPrice",
                    "node": {"type": "AtomicAccessor", "symbol": "AAPL", "field": "sma_20"}
                },
                {
                    "key": 2,
                    "symbol": "AAPL",
                    "field": "lastPrice",
                    "node": {"type": "AtomicAccessor", "symbol": "AAPL", "field": "sma_50"}
                }
            ]
        }
    },
    {
        "nl": "Give me a full MACD dashboard for Tesla: MACD line, signal, and histogram",
        "json": {
            "type": "subscribe",
            "requests": [
                {
                    "key": 1,
                    "symbol": "TSLA",
                    "field": "lastPrice",
                    "node": {"type": "AtomicAccessor", "symbol": "TSLA", "field": "macd_line"}
                },
                {
                    "key": 2,
                    "symbol": "TSLA",
                    "field": "lastPrice",
                    "node": {"type": "AtomicAccessor", "symbol": "TSLA", "field": "macd_signal"}
                },
                {
                    "key": 3,
                    "symbol": "TSLA",
                    "field": "lastPrice",
                    "node": {"type": "AtomicAccessor", "symbol": "TSLA", "field": "macd_histogram"}
                }
            ]
        }
    },

    # ------------------------------------------------------------------
    # TIER 8: Complex multi-node compositions
    # ------------------------------------------------------------------
    {
        "nl": "Show me the average bid-ask spread for AAPL over time",
        "json": {
            "type": "subscribe",
            "requests": [
                {
                    "key": 1,
                    "symbol": "AAPL",
                    "field": "lastPrice",
                    "node": {
                        "type": "Aggregate",
                        "arity": 2,
                        "inputs": [
                            {"type": "Listener", "symbol": "AAPL", "field": "ask"},
                            {"type": "Listener", "symbol": "AAPL", "field": "bid"}
                        ]
                    },
                    "pipeline": [
                        {"type": "Worker", "fn": "diff"},
                        {"type": "Worker", "fn": "mean"}
                    ]
                }
            ]
        }
    },
    {
        "nl": "I want to pairs-trade AAPL vs GOOG. Stream me the running mean of their price difference and the standard deviation of the spread",
        "json": {
            "type": "subscribe",
            "requests": [
                {
                    "key": 1,
                    "symbol": "AAPL",
                    "field": "lastPrice",
                    "node": {
                        "type": "Aggregate",
                        "arity": 2,
                        "inputs": [
                            {"type": "Listener", "symbol": "AAPL", "field": "lastPrice"},
                            {"type": "Listener", "symbol": "GOOG", "field": "lastPrice"}
                        ]
                    },
                    "pipeline": [
                        {"type": "Worker", "fn": "diff"},
                        {"type": "Worker", "fn": "mean"}
                    ]
                },
                {
                    "key": 2,
                    "symbol": "AAPL",
                    "field": "lastPrice",
                    "node": {
                        "type": "Aggregate",
                        "arity": 2,
                        "inputs": [
                            {"type": "Listener", "symbol": "AAPL", "field": "lastPrice"},
                            {"type": "Listener", "symbol": "GOOG", "field": "lastPrice"}
                        ]
                    },
                    "pipeline": [
                        {"type": "Worker", "fn": "diff"},
                        {"type": "Worker", "fn": "stddev"}
                    ]
                }
            ]
        }
    },
    {
        "nl": "Every 5 seconds, check AAPL's RSI and also stream its Bollinger bands continuously",
        "json": {
            "type": "subscribe",
            "requests": [
                {
                    "key": 1,
                    "symbol": "AAPL",
                    "field": "lastPrice",
                    "node": {
                        "type": "Interval",
                        "ms": 5000,
                        "child": {
                            "type": "AtomicAccessor",
                            "symbol": "AAPL",
                            "field": "rsi_14"
                        }
                    }
                },
                {
                    "key": 2,
                    "symbol": "AAPL",
                    "field": "lastPrice",
                    "node": {"type": "AtomicAccessor", "symbol": "AAPL", "field": "bollinger_upper"}
                },
                {
                    "key": 3,
                    "symbol": "AAPL",
                    "field": "lastPrice",
                    "node": {"type": "AtomicAccessor", "symbol": "AAPL", "field": "bollinger_lower"}
                }
            ]
        }
    },
    {
        "nl": "Build me a volatility monitor: stream AAPL's ATR, standard deviation of price, and the price range (high minus low) updated every 3 seconds",
        "json": {
            "type": "subscribe",
            "requests": [
                {
                    "key": 1,
                    "symbol": "AAPL",
                    "field": "lastPrice",
                    "node": {"type": "AtomicAccessor", "symbol": "AAPL", "field": "atr_14"}
                },
                {
                    "key": 2,
                    "symbol": "AAPL",
                    "field": "lastPrice",
                    "node": {"type": "Worker", "fn": "stddev"}
                },
                {
                    "key": 3,
                    "symbol": "AAPL",
                    "field": "lastPrice",
                    "node": {
                        "type": "Interval",
                        "ms": 3000,
                        "child": {
                            "type": "Aggregate",
                            "arity": 2,
                            "inputs": [
                                {"type": "Listener", "symbol": "AAPL", "field": "highPrice"},
                                {"type": "Listener", "symbol": "AAPL", "field": "lowPrice"}
                            ]
                        }
                    },
                    "pipeline": [{"type": "Worker", "fn": "diff"}]
                }
            ]
        }
    },
]


def format_examples_for_prompt() -> str:
    """Format examples as few-shot prompt entries."""
    import json
    lines = []
    for i, ex in enumerate(EXAMPLES, 1):
        lines.append(f"### Example {i}")
        lines.append(f"**User:** {ex['nl']}")
        lines.append(f"**Output:**\n```json\n{json.dumps(ex['json'], indent=2)}\n```\n")
    return "\n".join(lines)
