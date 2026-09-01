# Ractor::SharedVar

**One** typed variable that Ractors share, read and replace **without a lock**.
Every operation on it is a single atomic instruction on a single machine word:
nothing waits, nothing can be held, and there is nothing for a killed Ractor to
strand.

```ruby
require "ractor/shared_var"

var = Ractor::SharedVar.new(String, "hi".freeze)
var.set("omg".freeze)

r = Ractor.new(var) do |v|
  v.get            #=> "omg"
  v.set("wonderful".freeze)
end
r.join

var.get            #=> "wonderful"
```

## API

```ruby
var = Ractor::SharedVar.new(type, value)

var.get                            # the latest completed write, from any Ractor
var.set(value)                     # make it the latest; returns value
var.swap(value)                    # set, and return what was there
var.compare_and_swap(expected, value)  # set only if it still holds expected
var.compare_and_swap(expected, value) {|current, expected| bool }
                                   # ...only if the block says it matches
var.type                           # the Module every value must be
var.inspect
```

Five operations, and each one is a single bounded atomic — a load, a store, an
exchange, a compare-and-swap. Nothing in the library loops, which makes them
**wait-free**: not merely "somebody always makes progress", but *every* caller
finishes in a fixed number of instructions no matter what the others are doing.
The one thing that can stretch that bound is code you passed in yourself, the
block `compare_and_swap` takes.

## What is guaranteed

Every write to a `SharedVar` is a **release** and every read is an **acquire**.
Concretely:

* **`get` returns the most recently completed write**, whichever Ractor made it.
  There is no staleness window, no flush to wait for, and no such thing as a
  reader that keeps seeing an old value.
* **Every Ractor agrees on the order.** Two writes to the same variable are never
  seen in one order by one Ractor and the other order by another.
* **Which of two concurrent writers goes first is not decided by this library.**
  Two Ractors calling `set` at the same instant race, and the hardware picks.
  But the one that finishes first is first *for everybody*: its value is what
  every reader sees until the other one lands.
* **A write is all-or-nothing, and it publishes.** A reader sees the value before
  or the value after, never a mixture — and everything the writer did to build
  that object before storing it is finished and visible to whoever reads it. A
  freshly allocated, freshly frozen object can never be seen half-built.
* **A read never goes backwards.** Once a Ractor has seen a write, no later `get`
  in that Ractor can return anything earlier in the order.

What is *not* guaranteed is any relationship between two different variables.
Ordering holds per variable; two `SharedVar`s are two independent words.

```ruby
a = Ractor::SharedVar.new(Integer, 0)
b = Ractor::SharedVar.new(Integer, 0)
a.set(1); b.set(1)     # another Ractor may see b == 1 while a == 0
```

Changing several variables together is `Ractor::TVar`'s job, not this one's.

### Why release/acquire and not sequential consistency

The stronger thing a memory model can offer is *sequential consistency*: one
global order that every operation on every variable slots into. This library
deliberately does not buy it, because the only thing it adds is the ordering
between *different* variables that the paragraph above already declines to
promise — and it charges a full barrier on every write for it.

Everything in the list above survives the weaker model, and not by luck:

* **Per-variable order is not something sequential consistency provides.** Every
  atomic location has a single modification order that all threads agree on,
  whatever ordering its operations use. That is where "every Ractor agrees on the
  order" comes from.
* **A read-modify-write always acts on the latest value in that order.** Also
  true at every ordering, and it is what makes `swap` and `compare_and_swap`
  decisive rather than advisory — exactly one of eight racing `compare_and_swap`s
  can win.
* **Publication is precisely what release/acquire is for.** The release store
  puts the writer's work before the pointer; the acquire load puts the pointer
  before the reader's use of it.

So the practical difference is one pattern: two Ractors writing two *different*
variables, and two others reading both and disagreeing about which happened
first. That was never guaranteed here.

## The type

The type is fixed when the variable is made and checked on **every** write,
including writes from other Ractors:

```ruby
counter = Ractor::SharedVar.new(Integer, 0)
counter.set(1)
counter.type       #=> Integer
```

A value of the wrong type raises `TypeError` and the variable keeps what it had.
That is worth more here than it would be behind a lock: a lock-free variable has
no critical section in which to check an invariant, so the type is the one
invariant that can still be enforced, and it is enforced at the only moment that
matters — before the store.

Any `Class` or `Module` will do, `Object` included when you want anything, and
the check is `kind_of?`, so a subclass is fine.

## The value must be shareable

A `SharedVar` holds one **shareable** object, and so does everything written into
it. Anything else raises `ArgumentError`:

```ruby
box = Ractor::SharedVar.new(Hash, {}.freeze)
box.set({ a: 1 }.freeze)
box.get            #=> {a: 1}
```

That is what makes the variable safe to hand to any Ractor: it is frozen and
shareable itself, and so is what is inside it, so nothing reachable through it
can be mutated behind another Ractor's back. It also means a write **replaces**
the value rather than modifying it: `box.set(box.get.merge(k => v).freeze)`,
never `box.get[k] = v`.

A rejected write leaves the variable exactly as it was.

A string literal is **not** frozen unless the file says so, so this is the first
thing most code trips over:

```ruby
label = Ractor::SharedVar.new(String, "hi".freeze)
label.set("bye".freeze)
label.get          #=> "bye"
```

Put `# frozen_string_literal: true` at the top of the file and the `.freeze`
calls go away; `-"bye"` does the same thing for one literal.

## Read-modify-write

`get` is a snapshot: true when taken, possibly stale by the time you use it.
Reading it and writing back is the one thing a shared variable makes easy to get
wrong, and no amount of lock-freedom helps:

```ruby
# WRONG -- another write lands between the get and the set, and this discards it
n = counter.get
counter.set(n + 1)
```

`compare_and_swap` is what closes that gap. It writes **only if the variable still
holds what you read**, and tells you whether it did:

```ruby
state = Ractor::SharedVar.new(Symbol, :idle)
state.compare_and_swap(:idle, :running)   #=> true    # we claimed it
state.compare_and_swap(:idle, :running)   #=> false   # somebody else already had
```

So a read-modify-write is a `compare_and_swap` in a loop — read, compute, try, and
on a `false` start over from a fresh read:

```ruby
def increment(counter)
  loop do
    old = counter.get
    return old + 1 if counter.compare_and_swap(old, old + 1)
  end
end
```

**The loop is deliberately yours and not the library's.** A method that retried
for you would decide, on your behalf, that trying again forever is the right
answer to contention — and under real contention that is exactly the wrong answer
often enough to matter. Written out, the loop is where it belongs: you can cap
the attempts, back off, batch the work, give up and take a different path, or
report the failure upward. You can also see, in the code, that a contended write
costs a re-run of everything between the `get` and the `compare_and_swap`, which
is a good reason to keep that stretch short.

Four Ractors incrementing 500 times each:

```ruby
tally = Ractor::SharedVar.new(Integer, 0)
4.times.map do
  Ractor.new(tally) do |v|
    500.times { loop { old = v.get; break if v.compare_and_swap(old, old + 1) } }
  end
end.each(&:join)
tally.get          #=> 2000
```

### What counts as a match

By default `compare_and_swap` compares by **identity**, as `equal?` does, not `==`.
For symbols, integers and `true`/`false`/`nil` that is the same thing. For strings
and frozen containers it is not:

```ruby
one = "same".dup.freeze
two = "same".dup.freeze
tag = Ractor::SharedVar.new(String, one)
tag.compare_and_swap(two, "x".freeze)   #=> false
tag.compare_and_swap(one, "x".freeze)   #=> true
```

Identity is the right default, and not only because it is the cheap one: a value
that is `==` to what you read is not necessarily the value you read, so a retry
loop that accepted it would silently overwrite a write it never saw.

When you want some other comparison, pass a block. It is handed **the value that
is there and then the one you expected**, and a true return lets the write go
ahead:

```ruby
tag2 = Ractor::SharedVar.new(String, "same".dup.freeze)
tag2.compare_and_swap("same".dup.freeze, "x".freeze) { |current, expected| current == expected }
tag2.get           #=> "x"
```

`==`, `eql?`, a version check, a predicate on one field of a record — whatever
the block returns is what "still matches" means for that call. Some things worth
knowing about it:

* **It runs exactly once.** There is no retry loop for it to be re-run by, so
  unlike `Ractor::TVar` a side effect in it happens once, not once per attempt.
* **A write can still lose after the block approves it.** The store is a
  compare-and-swap against *the very object the block was shown*, so if another
  Ractor writes while your block is thinking, the call returns `false` — even if
  what they wrote would also have matched. That is deliberate: it keeps the
  block's verdict true of the object actually replaced, which is the whole point
  of a compare-and-set. Treat a `false` the same way you treat one from the
  identity form, and loop if that is your policy.
* **It is the one place Ruby runs mid-operation.** The block can allocate, raise
  and collect garbage. If it raises, the exception propagates and nothing was
  written.
* **`value` is type-checked before the block runs**, so a call that could never
  have stored anything does not get to run a block with a side effect in it.

`swap` sets and returns the previous value in one step, so of many Ractors
swapping, each is handed a value no other one is also handed — the way to drain
a variable exactly once, with no loop at all.

## What no lock buys

* **Nothing ever waits.** There is no queue, so a Ractor that is descheduled,
  paused at a breakpoint, or simply slow cannot hold anything up. A `get` costs
  the same whether or not another Ractor is writing.
* **Nothing can be stranded.** `Thread#kill`, an exception, `return`, `break`,
  `throw` — none of them can leave the variable in a state that blocks the next
  caller, because there is no state to leave. There is no `ensure` to arm and no
  window in which an interrupt is dangerous. Kill a Ractor in the middle of a
  retry loop and the variable is simply whatever the last completed write made it.
* **There is no lock order, so there is no deadlock.** Any variable may be touched
  at any point in a sequence of operations on any other, its own included.
  `Ractor::LockVar` has to refuse that; here there is nothing to refuse.
* **Reads scale.** Sixteen Ractors reading one variable do not interfere at all;
  see below.

## What it does not do

* **Several variables together.** Use `Ractor::TVar`.
* **Run your block under exclusion.** `compare_and_swap`'s block decides a match;
  it is not a critical section, and the write after it can still lose.
* **Retry for you.** There is no `update`. A read-modify-write is the loop above,
  and the policy in it is yours to write. If you want the pessimistic version
  instead — wait your turn, then run a block exactly once, side effects and all —
  that is `Ractor::LockVar#update`.
* **Notify anybody.** There is no waiting for a value to change; a reader that
  wants to know polls, or you use a `Ractor::Port`.
* **Order two different variables.** See *What is guaranteed*.

## Compared with its locking neighbours

| | `Ractor::SharedVar` | `Ractor::LockVar` | `Ractor::TVar` |
|---|---|---|---|
| synchronizes | one variable | one variable | several together |
| mechanism | atomic word | a lock | optimistic transaction |
| a caller can block | never | yes, waiting its turn | never |
| progress | wait-free | depends on the holder | lock-free |
| on conflict | tells you, and returns | waits | rolls back, runs again |
| retrying is | yours to write | not needed | done for you |
| its block | a predicate, optional | the work itself | the work itself |
| runs the block | exactly once | exactly once | as many times as it takes |
| side effects in it | fine — but the write may still fail after it | fine | no |
| touching another variable inside | fine | refused | that is the point |
| typed | yes | no | no |

The row that decides most cases is *on conflict*. If losing a race can be handled
— retry, back off, skip, take another path — this is the cheapest of the three by
a wide margin. If the work must happen exactly once and cannot be re-run, you want
the lock.

## Performance

The workload is one shared value, a frozen `{status:, seq:}` record. Numbers are
**nanoseconds per completed operation across all Ractors**, so one that halves
when the Ractors double means it scaled. Measured on an arm64 laptop, 8
performance cores, ruby 4.0.6; `benchmark/shared_var/scaling.rb` runs it and
checks afterwards that no write was lost. Each cell is the median of three runs.
The `cas loop` column is the Ruby-level retry loop from *Read-modify-write*, so it
includes the cost of re-running the loop body on every lost race.

### All Ractors on one variable

| Ractors | `SharedVar#get` | `LockVar#value` | `SharedVar` cas loop | `LockVar#update` |
|---:|---:|---:|---:|---:|
| 1 | 23 | 35 | 215 | 225 |
| 2 | 12 | 88 | 269 | 659 |
| 4 | 6 | 119 | 361 | 1451 |
| 8 | 3 | 423 | 648 | 3066 |

**Reading one shared variable scales perfectly and this is the number to know.**
Eight Ractors reading cost 3 ns each, roughly 8× *less* than one Ractor does,
because the reads run in parallel and an uncontended cache line is shared, not
passed around. The locking version goes the other way — 35 ns to 423, because
those eight readers form a queue — so at eight Ractors the gap is over 100×. The
`LockVar` cells here are the volatile ones, moving 10–30% between sweeps; the
`SharedVar` cells moved by under 10%.

**Contended writing does not scale, and nothing makes it.** One word can only be
written one at a time by anybody, so the retry loop climbs from 215 ns to 648: the
cache line is passed between cores on every write, and losers of the race run
their loop body again. It climbs about 5× more gently than the lock does, and it
climbs without ever blocking a caller, but it climbs. A single uncontended `set`,
for comparison, is 27 ns — the 215 is the read, the Hash allocation and the loop
around them, not the atomic.

### One variable each

| Ractors | `get` (ns) | `set` (ns) | cas loop (ns) |
|---:|---:|---:|---:|
| 1 | 24 | 27 | 216 |
| 2 | 12 | 14 | 133 |
| 4 | 6 | 13 | 103 |
| 8 | 5 | 10 | 100 |

Uncontended, everything scales to the machine's limit, the retry loop included —
a compare-and-swap that nobody is fighting over succeeds first time and is just a
write. **Give each Ractor a variable of its own whenever the problem allows it**,
exactly as with the locking classes; what is different here is that when it does
not allow it, reading still costs nothing.

## Implementation notes

* The variable is a struct of two words: the value and the type. The type is
  written once by `initialize` and never again; the value is only ever touched
  atomically: an acquire load for `get`, a release store for `set`, an
  acquire-release exchange for `swap`, and an acquire-release compare-and-swap —
  acquire on its failure path, which publishes nothing — for `compare_and_swap`.
* Ruby's public `RUBY_ATOMIC_*` macros hardcode sequential consistency, but the
  ordering is a parameter one layer down, on the `rbimpl_atomic_value_*` inline
  functions that `ruby/atomic.h` also ships. Those are the only internal names
  the extension uses, and it uses them behind a fallback: where they are absent
  the operations become the public sequentially consistent macros, which are
  strictly stronger and so still correct, just slower.
* On arm64 this is `ldapr` / `stlr` / `swpal` / `casal`; the load is the one that
  changes, since sequential consistency would need `ldar`. On x86-64 the win is
  on the other side — a release store is a plain `mov` where a sequentially
  consistent one is a locked `xchg`.
* Every method is one of those atomics with a type check in front of it. Nothing
  in the C loops, retries or waits, so every operation is **wait-free** — bounded
  for each caller individually, not just for the system as a whole. The only
  unbounded things in sight are the caller's own Ruby: a retry loop, and the
  block `compare_and_swap` takes.
* The block form is a load, a `rb_yield_values`, and a compare-and-swap against
  what the load returned. Yielding means arbitrary Ruby runs between the two, so
  that one path can allocate, trigger a GC, and be interrupted — none of which
  can corrupt anything, because the store is conditional on the slot still
  holding the loaded object and that object is live on the machine stack, which
  the GC scans conservatively, for as long as it is needed.
* The type check is `rb_obj_is_kind_of`, which walks the ancestry in C and
  dispatches no Ruby method. So a write cannot be interrupted between its check
  and the store that check guards, and a `Module` with a clever `===` does not
  get to run inside a write.
* The `T_DATA` is deliberately **not** `RUBY_TYPED_WB_PROTECTED`. A write barrier
  has to run as part of the store it belongs to, and there is no way to fuse one
  onto an atomic store that another Ractor may be racing. Left unprotected, the
  GC treats the variable as always remembered and re-marks it at the end of every
  incremental marking, which finds a value stored *during* the marking; declaring
  it protected would let such a value be swept while the variable still held it.
  The cost is one extra object marked per GC.
* Values are marked with `rb_gc_mark`, which pins, so compaction never moves the
  object the slot points at. That is what lets the slot be a bare word any Ractor
  may read at any moment without cooperating with the GC.
* Every method is safe to call from any Ractor (`rb_ext_ractor_safe`), and no
  method allocates on its own account, so the only operation that can trigger a
  GC part-way through is a `compare_and_swap` whose block allocates.

Part of [ractor-lockfree](../README.md).
