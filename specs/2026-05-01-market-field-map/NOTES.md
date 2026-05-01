# Discovery notes — market-field-map

Brief transcript of the /propose conversation on 2026-05-01.

## Confirmed dimensions

- **Target ticket:** ENC-35 (B1) from `contract-audit-2026-04-30-followups`.
- **Phasing:** depends on ENC-30 (A1: `ConfigNamespaceRegistry` wiring) landing first. User confirmed: "this can lean on enc-30".
- **Storage location at runtime:** type lives next to its consumer (`MarketTA.hpp` / `MarketTickComputer`), instance held by `MarketConnector` and threaded into the tick factory closure. User: "it can live alongside its peer only if that would work best."
- **Config-key namespace:** rename to `market.source.*`, with a one-release deprecation alias for `source.*`. Decision delegated to spec author by user ("you make that call").
- **Internal field-name renames:** none. Decision delegated by user ("you call on that too").

## Why this spec exists separately

ENC-35 was flagged in the contract-audit project description as the only ticket that explicitly requires `/propose` because the move cascades through Config, parser, default INI, MarketTA, MarketConnector, and tests — wider than a single mechanical rename.

## Skipped dimensions

- Performance budget — straightforward (no hot-path change).
- Authoring deadlines — none.
