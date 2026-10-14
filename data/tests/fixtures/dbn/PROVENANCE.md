# DBN fixtures — where these bytes came from, and under what terms

**These are real Databento DBN bytes. Nobody in this repo wrote them.** That is the whole
point: ENC-1482 exists to establish the DBN record layout and field names *by measurement*,
and a byte array we authored ourselves would make every conclusion about layout circular.

## Source

Copied verbatim from the test data of **[`databento/databento-cpp`]**, Databento's own
open-source C++ client, at `tests/data/` on `main` (fetched 2026-10-14):

| file here | upstream name | bytes | sha256 |
|---|---|---|---|
| `mbo.v1.dbn`    | `tests/data/test_data.mbo.v1.dbn`    | 318 | `cde22d5d4e87c242904adb6b7e3dd9b2f898d753d61fe87c526429a7de745796` |
| `mbo.v2.dbn`    | `tests/data/test_data.mbo.dbn`       | 465 | `31403d17bf9801842acf59bca668e6b8f19ff4b5d70dce05f0fe70d995fc70d6` |
| `mbo.v3.dbn`    | `tests/data/test_data.mbo.v3.dbn`    | 472 | `d2a526d952f845ab8f03ceac42ec828a2541e0b5e2e8a64df2e8bbaa1e898184` |
| `trades.v1.dbn` | `tests/data/test_data.trades.v1.dbn` | 302 | `1c5ca1e96729ba941ee4796265e18e47f03d2dba67d098c90acf3aa39a584c2e` |
| `trades.v2.dbn` | `tests/data/test_data.trades.dbn`    | 449 | `6ef751abbd186b862afd9c65fd88dd54585587aa8cc3a059bc78305125efce3c` |

[`databento/databento-cpp`]: https://github.com/databento/databento-cpp

Renamed on the way in only to put the DBN version in the name — the bytes are untouched,
which the sha256 column lets you check against upstream.

## Licensing

`databento-cpp` is **Apache License 2.0** (`LICENSE` at the repo root), which permits
redistribution of the work and of derivative works with attribution and a copy of the
licence notice. These five files are test data distributed as part of that Apache-2.0 work.
No Databento API key, no subscription and no paid historical pull was involved, and none is
required to obtain them. Attribution is this file.

They are a **public sample**, not our market data: 2,006 total bytes, two records each, one
instrument (`ESH1` — the March-2021 E-mini S&P future), one dataset-day (2020-12-28) of
`GLBX.MDP3`.

## What is NOT here, and why

The ticket asked for "a few thousand records". **Databento publishes no free sample DBN file
of that size**, and there is no `DATABENTO_*` key in this environment — ENC-1483's 7-day
paid pull is blocked on one. Two records per schema is what is freely available, so that is
what is committed. The deliberate choice was a small *real* fixture over a large fabricated
one.

The consequence, stated plainly: **these files cover exactly one `Action` (`C`, Cancel) and
one `Side` (`A`, Ask).** They cannot exercise the rest of the action vocabulary. So
`DatabentoAdapterTest.cpp` carries a minimal DBN **encoder**, and the first thing that test
asserts is that the encoder reproduces each of these five files **byte for byte** from the
values tabulated below — which are written into the test as literals and taken from
`databento-cpp`'s own decoder-test expectations, *not* read back out of our decoder. Only
then does it use that encoder to build a stream covering all seven actions and all three
sides.

That ordering is what keeps the all-actions coverage non-circular. The layout, the field
names and the framing come from these bytes and from
[`databento-cpp`'s own decoder tests][vendor-tests], which assert the same values
independently; the encoder is an instrument *validated against* them, not a substitute for
them.

[vendor-tests]: https://github.com/databento/databento-cpp/blob/main/tests/src/dbn_decoder_tests.cpp

## The values in them (cross-checked against the vendor's own tests)

`databento-cpp`'s `DbnDecoderSchemaTests.TestDecodeMbo` / `TestDecodeTrades` assert these
same numbers against the same files, so the expectations in our test have a second,
independent source.

Metadata, identical in all five except `version`, `schema` and `symbol_cstr_len`:

```
dataset        "GLBX.MDP3"
schema         0 (Mbo) / 4 (Trades)
start          1609160400000000000      end  1609200000000000000
limit          2                        ts_out 0
stype_in       1 (RawSymbol)            stype_out 0 (InstrumentId)
symbol_cstr_len  22 (v1) / 71 (v2, v3)
symbols        ["ESH1"]   partial []   not_found []
mappings       ESH1 -> [20201228, 20201229) = "5482"
```

`mbo.*.dbn` — two `MboMsg` (`rtype` 0xA0, `length` 14 words = 56 bytes):

```
ts_event 1609160400000429831  order_id 647784973705  price 3722750000000  size 1
flags 128  channel_id 0  action 'C'  side 'A'  ts_recv 1609160400000704060
ts_in_delta 22993  sequence 1170352

ts_event 1609160400000431665  order_id 647784973631  price 3723000000000  size 1
flags 128  channel_id 0  action 'C'  side 'A'  ts_recv 1609160400000711344
ts_in_delta 19621  sequence 1170353
```

`trades.*.dbn` — two `TradeMsg` (`rtype` 0x00, `length` 12 words = 48 bytes):

```
ts_event 1609160400098821953  price 3720250000000  size 5   action 'T' side 'A'
flags 129  depth 0  ts_recv 1609160400099150057  ts_in_delta 19251  sequence 1170380

ts_event 1609160400107665963  price 3720250000000  size 21  action 'T' side 'A'
flags 129  depth 0  ts_recv 1609160400108142648  ts_in_delta 20728  sequence 1170414
```

Note the prices: `3722750000000` at the global 1e-9 scale is 3722.75, an exact quarter-tick
ES price. It arrives as `int64` and stays `int64` — SPEC D8's "no double round trip" is a
property of the provider's own encoding, not something the adapter has to achieve.

## Facts these files settle, and facts they do not

**Settled by measurement** (and recorded in `specs/2026-10-14-gma-data-layer/SPEC.md` §5):
the DBN magic and version byte, the 100-byte fixed metadata block and its field order, the
v1 extra `record_count`, the v2+ `symbol_cstr_len`, the 53/47-byte reserved region, the
variable-length symbol and mapping sections, **DBN v3's 8-byte alignment padding at the end
of the metadata** (`mbo.v3.dbn` carries 7 zero bytes there and `mbo.v2.dbn` carries none),
the 16-byte record header, and the exact field order and offsets of `MboMsg` and
`TradeMsg` — which are **byte-identical across v1, v2 and v3** (the vendor aliases
`v1::MboMsg = v2::MboMsg = v3::MboMsg = MboMsg`, and these files confirm it).

**Not settled, and not settleable from a sample this small:** whether `instrument_id` is
stable within a dataset-day (one id in one file proves nothing — that is ENC-1483/T5), and
whether a DBN `Fill` is accompanied by a separate `Cancel`/`Modify` carrying the resting
order's reduction. See the `fillReducesRestingOrder` note in `DatabentoAdapter.hpp`.
