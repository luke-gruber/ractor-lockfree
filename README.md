# ractor-lockfree

State that Ractors share **without a lock**: a variable, and a hash table.

## Ractor::LockFree::Var

One typed, shareable value. Any Ractor may read it or replace it, and no
operation on it can ever block another one: each is a single atomic instruction
on a single machine word.

```ruby
require "ractor/lockfree"

var = Ractor::LockFree::Var.new(String, "hi".freeze)
var.set("hello".freeze)

r = Ractor.new(var) do |v|
  v.get            #=> "hello"
  v.set("goodbye".freeze)
end
r.join

var.get            #=> "goodbye"
```

* `get` returns the **most recently completed write**, from whichever Ractor made
  it. No staleness window.
* Two Ractors writing at the same instant **race**, and the library does not
  decide who wins — but whoever finishes first is first **for everybody**, and
  every Ractor agrees on the order.
* A write **publishes**: everything the writer did to build the object is
  finished before any reader can see the pointer to it, so a freshly frozen
  object is never seen half-built. Writes are release stores and reads are
  acquire loads — [and that is all this library asks
  for](docs/var.md#why-releaseacquire-and-not-sequential-consistency).
* A read-modify-write is a `compare_and_swap` loop, and the loop is **yours**, so
  you decide what a lost race costs:

```ruby
tally = Ractor::LockFree::Var.new(Integer, 0)
4.times.map do
  Ractor.new(tally) do |v|
    500.times { loop { old = v.get; break if v.compare_and_swap(old, old + 1) } }
  end
end.each(&:join)
tally.get          #=> 2000
```

* `compare_and_swap` matches by identity by default. Pass a block and the match is
  whatever you say — `==`, `eql?`, a check on one field — while the write stays
  conditional on the exact object the block was shown:

```ruby
config = Ractor::LockFree::Var.new(Hash, { gen: 1, host: "a".freeze }.freeze)
nxt = { gen: 2, host: "b".freeze }.freeze

config.compare_and_swap({ gen: 1 }, nxt) { |cur, exp| cur[:gen] == exp[:gen] }   #=> true
config.get         #=> {gen: 2, host: "b"}
```

**[Full documentation: `Ractor::LockFree::Var`](docs/var.md)** — the ordering
guarantees, what the type is for, how to write the retry loop, and measurements
against the locking equivalent.

## Ractor::LockFree::Hash

A hash table many Ractors read and write at once. Two operations — `get` and
`put` — and no lock anywhere: a write touches one key's word, and a read of a
key nobody is writing does not touch anything at all.

```ruby
require "ractor/lockfree"

routes = Ractor::LockFree::Hash.new
routes.put("/health".freeze, :ok)

Ractor.new(routes) { |h| h.put("/users".freeze, :index) }.join

routes.get("/users".freeze)      #=> :index
routes.get("/nope".freeze)       #=> nil
```

* Keys and values must be **shareable**, like everything else Ractors pass
  around, and the table itself is frozen and shareable. Keys are matched the way
  `Hash` matches them, by `#hash` and `#eql?`.
* `get` takes a **default** for a key that is not there, which is also how a
  stored `nil` is told from a missing key — pass a sentinel the table cannot be
  holding. It must be shareable too, since a caller cannot tell whether what
  came back was stored or defaulted:

```ruby
NOTHING = Object.new.freeze

memo = Ractor::LockFree::Hash.new
memo.put(:looked_up, nil)

memo.get(:looked_up)                          #=> nil
NOTHING.equal?(memo.get(:looked_up, NOTHING)) #=> false
NOTHING.equal?(memo.get(:not_yet, NOTHING))   #=> true
```

* **Reads scale perfectly, and reading one hot key scales best of all** — eight
  Ractors reading the same key cost 5 ns each where one Ractor costs 34, because
  a cache line nobody writes is free to share. That is what the table is for: a
  registry or cache that many Ractors read constantly and add to occasionally.
* It **grows without a lock and without stopping anybody.** The table doubles at
  three-quarters full, and the callers passing through carry the old slots into
  the new table a batch at a time — writers 64 slots, readers 8 — so no caller
  ever waits for a resize.
* There is **no `delete`, no `each`, no `size`**, and no conditional write. A key
  that has been stored is stored for good. When a value has to be updated from
  its own previous value, store a `Ractor::LockFree::Var` and use its
  `compare_and_swap`: a lock-free table of keys to lock-free cells.

```ruby
hits = Ractor::LockFree::Hash.new
hits.put(:home, Ractor::LockFree::Var.new(Integer, 0))

cell = hits.get(:home)
4.times.map do
  Ractor.new(cell) do |c|
    250.times { loop { n = c.get; break if c.compare_and_swap(n, n + 1) } }
  end
end.each(&:join)

hits.get(:home).get              #=> 1000
```

**[Full documentation: `Ractor::LockFree::Hash`](docs/hash.md)** — what a reader
can and cannot see, what growing costs, how it compares with a `Hash` behind a
lock, and the measurements.

## Why lock-free

Reading shared state from many Ractors is cheaper than taking a lock — and it is
the thing Ractors do most.

Nothing waits. A reader cannot be held up by a writer, a writer cannot be held up
by a Ractor that was descheduled mid-operation, and nothing can be stranded: a
`Thread#kill`, an exception or a `return` out of a retry loop leaves the state
exactly as it was and the next caller unaffected. There is no lock order, so
there is no deadlock.

`LockFree::Var` goes further than lock-free and is **wait-free** — nothing in it
loops, so every operation is one bounded atomic (`ldapr` / `stlr` / `casal` on
arm64, a plain `mov` for a store on x86). `LockFree::Hash` is lock-free but not
wait-free: a `put` can be made to retry by another `put`, and a caller may be
handed a batch of migration work. Somebody always makes progress and nobody can
be blocked, but a single call has no fixed bound.

What you give up in both is atomicity across more than one thing at a time — one
variable, one key — and the handling of a lost race, which is yours rather than
the library's. See the comparison tables for
[`LockFree::Var`](docs/var.md#compared-with-its-locking-neighbours) and
[`LockFree::Hash`](docs/hash.md#compared-with-its-neighbours).

## Installation

```
gem install ractor-lockfree
```

Requires Ruby 4.0 or newer, and builds a small C extension.

## Development

```
bundle install
bundle exec rake              # compile, then run every test and doc example
ruby benchmark/lockfree_var/scaling.rb
ruby benchmark/lockfree_hash/scaling.rb
```

## Prior art

This is the lock-free corner of the same problem
[ractor-sharing](https://github.com/ko1/ractor-sharing) covers with locks and
with software transactional memory; `Ractor::LockVar` and `Ractor::TVar`. Different
problems require different tools.

## License

MIT.
