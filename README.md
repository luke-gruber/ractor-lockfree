# ractor-lockfree

A variable that Ractors share **without a lock**.

`Ractor::SharedVar` holds one typed, shareable value. Any Ractor may read it or
replace it, and no operation on it can ever block another one: each is a single
atomic instruction on a single machine word.

```ruby
require "ractor/shared_var"

var = Ractor::SharedVar.new(String, "hi".freeze)
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
  for](docs/shared_var.md#why-releaseacquire-and-not-sequential-consistency).
* A read-modify-write is a `compare_and_swap` loop, and the loop is **yours**, so
  you decide what a lost race costs:

```ruby
tally = Ractor::SharedVar.new(Integer, 0)
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
config = Ractor::SharedVar.new(Hash, { gen: 1, host: "a".freeze }.freeze)
nxt = { gen: 2, host: "b".freeze }.freeze

config.compare_and_swap({ gen: 1 }, nxt) { |cur, exp| cur[:gen] == exp[:gen] }   #=> true
config.get         #=> {gen: 2, host: "b"}
```

**[Full documentation: `Ractor::SharedVar`](docs/shared_var.md)** — the ordering
guarantees, what the type is for, how to write the retry loop, and measurements
against the locking equivalent.

## Why lock-free

Reading one shared variable from many Ractors is cheaper than taking a lock.

Nothing waits, and nothing in the library loops: every operation is one bounded
atomic — `ldapr` / `stlr` / `casal` on arm64, a plain `mov` for a store on x86 —
so it is **wait-free**, not merely lock-free. Nothing can be stranded either —
a `Thread#kill`, an exception or a `return` out of a retry loop leaves the
variable exactly as it was and the next caller unaffected. There is no lock
order, so there is no deadlock.

What you give up is that a lost race is yours to handle rather than the
library's, and that ordering holds for one variable at a time. See
[the comparison table](docs/shared_var.md#compared-with-its-locking-neighbours).

## Installation

```
gem install ractor-lockfree
```

Requires Ruby 4.0 or newer, and builds a small C extension.

## Development

```
bundle install
bundle exec rake              # compile, then run every test and doc example
ruby benchmark/shared_var/scaling.rb
```

## Prior art

This is the lock-free corner of the same problem
[ractor-sharing](https://github.com/ko1/ractor-sharing) covers with locks and
with software transactional memory; `Ractor::LockVar` and `Ractor::TVar`. Different
problems require different tools.

## License

MIT.
