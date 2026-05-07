#pragma once

namespace gma {

// Register engine-provided node builders (Listener, Worker, Aggregate, Interval,
// BucketTime, AtomicAccessor, GroupSplit, Chain) with NodeTypeRegistry. The
// "SymbolSplit" JSON wire name is retained as an alias for GroupSplit for
// backward compatibility. Idempotent on duplicates — safe to call multiple
// times.
void registerBuiltinNodeTypes();

} // namespace gma
