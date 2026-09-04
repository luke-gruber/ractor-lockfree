# Ractor::LockFree::Hash

A hash table that Ractors share, read and write **without a lock**. Any number
of Ractors may look keys up and store into it at the same instant; no operation
on it can block another one, and there is nothing for a killed Ractor to strand.

```ruby
require "ractor/lockfree"

h = Ractor::LockFree::Hash.new
h.put(:host, "example.com".freeze)

r = Ractor.new(h) do |hash|
  hash.get(:host)        #=> "example.com"
  hash.put(:port, 443)
end
r.join

h.get(:port)             #=> 443
h.get(:missing)          #=> nil
```

Two operations, no iteration and no removal. It is the shared **registry** or
**cache** — a table many Ractors read constantly and add to occasionally — and
not a general-purpose `Hash`.

## API

```ruby
h = Ractor::LockFree::Hash.new

h.get(key)             # the value stored for key, or nil
h.get(key, default)    # ...or default, since nil is a value you can store
h.put(key, value)      # store; returns value
```

Keys, values and defaults must all be **shareable**. Keys are matched the way
`Hash` matches them, by `#hash` and `#eql?`.

## What is guaranteed

Every `put` is a **release** and every `get` is an **acquire**, on the one word
that holds that key's value. Concretely, and per key:

* **`get` returns the most recently completed `put`**, whichever Ractor made it.
  There is no staleness window and no flush to wait for.
* **Every Ractor agrees on the order of the writes to a key.** Two `put`s to the
  same key are never seen in one order by one Ractor and the other order by
  another.
* **A value never goes backwards.** Once a Ractor has seen `v`, no later `get`
  from any Ractor returns a value that `v` replaced — not while the table is
  growing, not while another Ractor is halfway through a write.
* **Which of two concurrent writers to one key wins is not decided by this
  library.** They race, and the hardware picks; but whoever completes last is
  what *everybody* then reads.
* **A key never comes back as a different key's value.** A reader either finds
  the key with a value that was really stored for it, or does not find it.
* **A `put` publishes.** Everything the writer did to build the value is finished
  before any reader can see the pointer to it, so a freshly frozen object is
  never seen half-built.
* **A `get` never fails, and never gets in a writer's way.** Reading a key while
  the whole table is being copied out from under it costs a probe in each of the
  two tables, and nothing more.

What is **not** guaranteed is an order between *different* keys. If one Ractor
does `put(:a, 1)` then `put(:b, 2)`, another Ractor may see `:b` and not `:a`.
Each key is its own atomic word; the table is not a snapshot, and there is no
moment at which it is "consistent" across keys. If two entries have to change
together, they belong in one frozen value under one key — or in a
`Ractor::TVar`.

A key in flight is briefly **not there**: a `put` of a key nobody has stored
before claims the slot first and stores the value after, so between those two
instants a reader gets the default for it. That is a key that has not been
written yet, and it is the same answer the reader would have got a nanosecond
earlier. What a reader cannot see is the slot's *previous* content, because a
slot never has any: keys and values are only ever added.

## Keys and values must be shareable

Everything that goes in is a **shareable** object, and so is the table itself,
which is what makes it safe to hand to any Ractor. Anything unshareable raises
`ArgumentError`, before the table is touched, so a rejected write leaves it
exactly as it was:

```ruby
conf = Ractor::LockFree::Hash.new
conf.put(:limits, { rps: 100 }.freeze)
conf.get(:limits)        #=> {rps: 100}
```

A string literal is not frozen unless the file says so, which is the first thing
most code trips over:

```ruby
cache = Ractor::LockFree::Hash.new
cache.put("key".freeze, "value".freeze)
cache.get("key".freeze)  #=> "value"
```

Put `# frozen_string_literal: true` at the top of the file and the `.freeze`
calls go away; `-"key"` does the same for one literal.

`get` checks the key too, so looking a key up needs a shareable key as much as
storing one does — and it checks the default, whether or not it ends up
returning it, because a caller cannot tell whether what came back was stored or
defaulted, so both have to be equally safe to hand to another Ractor. Symbols,
numbers, `nil`, `true`, `false`, frozen strings and frozen containers of
shareable things are all fine.

A stored value **replaces** the old one rather than being modified in place:
`h.put(k, h.get(k).merge(x: 1).freeze)`, never `h.get(k)[:x] = 1` — which would
raise anyway, the value being frozen.

## Telling a stored nil from a missing key

`nil` is a perfectly good value to store, so a bare `get` is ambiguous: it
answers `nil` both for a key that holds `nil` and for a key that is not there.
The second argument is what resolves that — it is what comes back when the table
has no such key, so pass something the table cannot be holding and compare:

```ruby
MISSING = Object.new.freeze     # shareable, and nothing else can be it

seen = Ractor::LockFree::Hash.new
seen.put(:answered, nil)

seen.get(:answered)                          #=> nil
seen.get(:answered, MISSING)                 #=> nil
MISSING.equal?(seen.get(:never, MISSING))    #=> true
MISSING.equal?(seen.get(:answered, MISSING)) #=> false
```

A frozen bare `Object` is the usual sentinel, and a private constant is the
usual place for it: nothing a caller can construct is `equal?` to it, so the
answer is never ambiguous. When the values are known to be of one kind, anything
outside that kind does as well — `:missing` in a table of strings, `-1` in a
table of counts.

Use the one-argument form when a missing key and a stored `nil` mean the same
thing to you, which is most of the time, and a sentinel when they do not — a
memo table whose answers may be `nil`, a "have I handled this?" set, a cache
that stores negative results.

The default is also the shortest way to write "or else this", with no branch at
all:

```ruby
limits = Ractor::LockFree::Hash.new
limits.put(:rps, 100)

limits.get(:rps, 10)            #=> 100
limits.get(:burst, 10)          #=> 10
```

It is only a default, though, and never a write: nothing is stored for a key
that was not there, and two Ractors defaulting the same key both just get the
default. There is no `fetch_or_store` — see *Read-modify-write*.

## Read-modify-write

There is no compare-and-swap here, and so **no way to make a `put` conditional**.
Read a value, compute from it, and store it back, and a `put` from another Ractor
that landed in between is gone:

```ruby
# WRONG -- another Ractor's increment lands between the get and the put
n = counter.get(:hits)
counter.put(:hits, n + 1)
```

`Ractor::SharedVar` is what closes that gap, and it is shareable, so it can be
the value:

```ruby
hits = Ractor::LockFree::Hash.new
hits.put(:home, Ractor::SharedVar.new(Integer, 0))

cell = hits.get(:home)
4.times.map do
  Ractor.new(cell) do |c|
    250.times { loop { old = c.get; break if c.compare_and_swap(old, old + 1) } }
  end
end.each(&:join)

hits.get(:home).get      #=> 1000
```

The table then holds the *identity* of each counter, which never changes, and
each counter holds a word that any Ractor can update without losing a write.
That split is the shape most shared state wants: a lock-free table of keys to
lock-free cells.

If a key's value only ever moves in one direction — a generation number, a
"ready" flag, a memoized answer that is always the same answer — a bare `put`
needs none of this. Two Ractors computing the same memo and both storing it are
not a lost update; they stored the same thing.

## Growing

The table starts at 16 slots and doubles when it is three-quarters full. There is
no lock and no stop-the-world, and no caller waits for the resize: the Ractor
that fills the table allocates the successor and copies a bounded batch of slots
into it, and every `put` after that copies another batch on its way through.
Reads help too — a `get` that had to look in the successor carries a few slots
across, so a table that stops being written still finishes migrating instead of
leaving every later lookup to search two tables forever.

The consequence to know is that **`put` is amortized, not bounded**. Most calls
are a probe and a compare-and-swap; the ones that land during a migration also
copy up to 64 slots, and the one that trips the resize allocates the new table.
Nothing blocks, but the cost is not flat. Fill the table before handing it to the
readers if you want it to be.

Growing is the only thing that allocates. The old table is dropped when the last
slot has been carried, and freed by the GC once no Ractor is still reading it.

## What no lock buys

* **Nothing ever waits.** There is no queue, so a Ractor that is descheduled,
  paused at a breakpoint, or simply slow cannot hold anything up. A `get` of one
  key costs the same whether or not another Ractor is writing a different one —
  or the same one.
* **Nothing can be stranded.** `Thread#kill`, an exception, `return`, `throw` —
  none of them can leave the table in a state that blocks the next caller. Kill a
  Ractor mid-`put` and the table holds either the old value or the new one, and
  the next caller cannot tell which of those two it was.
* **There is no lock order, so there is no deadlock.** Any key may be touched at
  any point in a sequence of operations on any other key, or on any other table.
* **Reads scale.** Ractors reading different keys never touch the same cache
  line; Ractors reading the *same* key share one, and sharing a clean line is
  free. See [Performance](#performance).

## What it does not do

* **Delete.** A key that has been stored is stored for the life of the table.
  This is a registry and a cache, not a working set — if keys churn without
  bound, memory grows without bound. (Deleting from a lock-free open-addressed
  table means tombstones, and tombstones mean a compaction pass that every
  reader has to cooperate with. It is not free, and nothing here needs it yet.)
* **Iterate, count, or hand you a `Hash`.** There is no `each`, `size`, `keys` or
  `to_h`. A traversal of a table that other Ractors are writing has to define
  what it means, and none of the definitions are cheap. Keep the key list on the
  side if you need one — a frozen array in a `Ractor::SharedVar`, say.
* **Conditional or atomic read-modify-write.** No `compare_and_swap`, no
  `update`, no `fetch_or_store`. See *Read-modify-write*.
* **Order two different keys.** See *What is guaranteed*.
* **Notify anybody.** A reader that wants to know when a key appears polls, or
  you use a `Ractor::Port`.
* **A stored default, a `default_proc`, or a `fetch` that raises.** `get`'s
  second argument is a default for that one call, not a property of the table
  and not a write; there is nothing that computes a missing value for you, and
  nothing that raises when a key is absent.

## Compared with its neighbours

| | `Ractor::LockFree::Hash` | `Hash` behind a `Mutex` | `Ractor::LockVar` of a frozen `Hash` | `Ractor::TVar` |
|---|---|---|---|---|
| shared across Ractors | yes | no — a `Hash` is unshareable | yes | yes |
| a caller can block | never | yes, waiting its turn | yes | never |
| progress | lock-free | depends on the holder | depends on the holder | lock-free |
| a write costs | one key's word | the whole table | a copy of the whole table | a transaction |
| readers interfere | no | yes, they queue | no, but they see a snapshot | no |
| read-modify-write | yours to arrange | the critical section | `update` | the transaction |
| iterate, delete, size | no | yes | yes | yes |
| several keys atomically | no | yes | yes | yes |

The first row decides more cases than it looks like it should: a plain `Hash` is
not shareable, so "put a `Mutex` around it" is not an option *between* Ractors at
all — only between threads inside one. Among the three that are shareable, the
question is whether writes replace the whole table (`LockVar`, `TVar`) or one
key (here). Replacing the whole table gives you atomic multi-key updates and
iteration, and costs a copy per write. One key at a time costs nothing per write
and gives you neither.

## Performance

The workload is a shared table of symbol keys to one small frozen value, driven
from 1 to 8 Ractors. Each Ractor cycles over 1000 keys of its own, or all of
them share one hot key. Numbers are **nanoseconds per completed operation across
all Ractors**, so one that halves when the Ractors double means it scaled.
Measured on an arm64 laptop, 8 performance cores, ruby 4.0.6;
`benchmark/lockfree_hash/scaling.rb` runs it and checks afterwards that no key
was lost. Each cell is the median of three runs.

| Ractors | `get`, key each | `get`, one hot key | `put`, key each | `put`, one hot key |
|---:|---:|---:|---:|---:|
| 1 | 41 | 34 | 44 | 36 |
| 2 | 24 | 17 | 29 | 31 |
| 4 | 13 | 9 | 19 | 47 |
| 8 | 7 | 5 | 16 | 48 |

The same loop against a plain, unshared, unlocked `Hash` in one Ractor costs
37 ns for `[]` and 45 ns for `[]=`, and 21 ns of every cell is the benchmark's own
loop — an array index and a modulo — which is worth knowing before reading much
into the single-Ractor row.

**Passing a default costs nothing measurable**, whatever it is: a symbol, a
frozen `Object` sentinel and a deeply frozen `Hash` all land within the noise of
a `get` without one. Ruby caches shareability on the object the first time it is
checked, so the check on the second and every later call is a flag test.

**Reading scales perfectly, and reading one hot key scales best of all.** Eight
Ractors on one key cost 5 ns each against one Ractor's 34: they share a single
cache line, nobody writes it, and a clean shared line is free to read on every
core at once. Different keys are slightly slower only because they touch more
memory. **This is the number to know** — one shared table that many Ractors read
constantly is what this data structure is for, and it costs about what a private
`Hash` lookup costs, with nothing to lock and no copy to hand around.

**Writing different keys scales, but not perfectly** — 44 ns down to 16, about
2.7× for 8× the Ractors. Two different keys are two different words, and the
compare-and-swaps on them never meet; what they do share is a cache line, since
a slot is three words and several slots fit in 64 bytes. So writers that never
touch the same key still pass lines between cores. Spreading hot keys apart is
not something the caller can arrange, which makes this the structure's real
ceiling on write-heavy work.

**Writing one key does not scale, and nothing makes it.** One word can only be
written one at a time by anybody, so the hot-key column turns around after two
Ractors and settles around 48 ns — and it is the one volatile column, moving
50% between sweeps, because it is a cache line being fought over. It climbs
gently and it never blocks a caller, but if every Ractor writes the same key,
give them a key each, or a `Ractor::SharedVar` each, and combine afterwards.

## Implementation notes

* A slot is three words — the key's hash, the key, and the value — in one flat
  array whose length is a power of two, probed triangularly (`i += d; d += 1`)
  from the hash. This is the layout of Ruby's own `concurrent_set.c`, with a
  value word added.
* Each of the three words only ever moves **once**, in one direction:
  `0 -> hash`, `undef -> key`, and `undef -> value -> value' -> ... -> MOVED`.
  Every transition is a compare-and-swap, so a Ractor that loses a race learns
  what the winner wrote and carries on from there; nothing is ever un-written,
  which is why a reader that arrives mid-write sees either a key that is not
  there yet or a value that really was stored.
* The hash word is claimed before the key, so a probe can skip a slot belonging
  to a different hash without touching — and without having to `eql?` — the key.
  `#hash` and `#eql?` are called on the caller's key, which means Ruby runs
  during a lookup; `#hash` is called once, before any slot is read.
* `Qundef` is the "nothing here yet" sentinel rather than `0`/`false`, because
  `nil`, `false` and `true` are all legal keys and values. `MOVED` is a private
  frozen object no caller can name, so it cannot collide with a real value
  either.
* **Resizing is cooperative**, which it has to be: `concurrent_set.c` takes the
  VM lock to resize, and an extension has no VM lock to take. Instead, the
  Ractor that finds the table three-quarters full compare-and-swaps a successor
  into place; from then on every caller passing through carries a batch of slots
  across (64 for a write, 8 for a read that had to consult the successor) and
  claims that batch with one `fetch_add`, so no two Ractors carry the same slot.
  A slot is carried by copying its value into the successor and *then*
  compare-and-swapping the old value to `MOVED`; if that fails, a newer value
  landed and is carried again. A write to a `MOVED` slot cannot succeed, so it
  retries in the successor. The current table is retired only when every slot is
  sealed, so a table a caller is holding is always either current or one whose
  sealed slots point the way forward.
* **Retired tables are reclaimed by the GC, not by the algorithm.** A Ractor may
  still be reading a table that has just been retired, and the usual lock-free
  answers to that (hazard pointers, epochs) all need every reader to announce
  itself. Ruby already has something better: the table is a Ruby object, the
  reader holds it in a C local that `RB_GC_GUARD` keeps on the machine stack, and
  the GC scans machine stacks conservatively. So the table stays live for exactly
  as long as somebody is looking at it, with no bookkeeping in the read path at
  all.
* Ruby's public `RUBY_ATOMIC_*` macros hardcode sequential consistency, but the
  ordering is a parameter one layer down, on the `rbimpl_atomic_value_*` inline
  functions `ruby/atomic.h` also ships. Those are the only internal names the
  extension uses, and behind a fallback: where they are absent the operations
  become the public sequentially consistent macros, which are strictly stronger
  and so still correct, just slower.
* Keys and values are marked with `rb_gc_mark`, which pins, so compaction never
  moves an object a slot points at. That is what lets a slot be a bare word any
  Ractor may read at any moment without cooperating with the GC.
* The `T_DATA`s are deliberately **not** `RUBY_TYPED_WB_PROTECTED`. Unprotected,
  the table counts as always remembered: every marking walks the slots again, so
  a key or value stored *after* that walk began is still found. Protecting it
  would mean a write barrier on the table after every successful slot CAS, which
  buys cheaper marking at the price of a call on the write path — and a store
  whose barrier is wrong loses an object a slot still holds. On a ruby whose
  local GC pins shareable objects the question is moot once there is more than
  one Ractor, since every key and value is shareable and pinned on its own; it is
  not moot in a single-Ractor program, nor on a ruby without that pinning.
* The table object is frozen and shareable, and so is every internal table
  (`RUBY_TYPED_FROZEN_SHAREABLE`). Every method is safe to call from any Ractor
  (`rb_ext_ractor_safe`), and the read path allocates nothing at all: `get`
  returns either a `VALUE` out of a slot or the default it was handed.
* It is **lock-free, not wait-free**, and that is the difference from
  `Ractor::SharedVar`: a `put` can be made to go round its loop again by another
  Ractor's `put`, and a caller can be handed migration work. Some caller always
  makes progress, and no caller can be blocked by one that stopped — but no
  individual call has a fixed instruction bound.

Part of [ractor-lockfree](../README.md).
