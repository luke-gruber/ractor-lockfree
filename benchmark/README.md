# Benchmarks

```
ruby benchmark/lockfree_var/scaling.rb [max_ractors]
ruby benchmark/lockfree_hash/scaling.rb [max_ractors]
```

`lockfree_var/scaling.rb` runs the same workload — one shared frozen
`{status:, seq:}` record — across 1, 2, 4, 8 and 16 Ractors, first with every
Ractor on **one shared variable** and then with **one variable each**, and
reports nanoseconds per completed operation across all Ractors. A number that
halves when the Ractors double means the operation scaled.

It finishes by checking that a contended `compare_and_swap` retry loop lost no
writes, so a run that prints `LOST WRITES` is a bug and not a slow machine.

If [ractor-sharing](https://github.com/ko1/ractor-sharing) is installed it also
prints the same workload against the locking `Ractor::LockVar`, which is the
comparison quoted in [docs/var.md](../docs/var.md#performance).

`lockfree_hash/scaling.rb` runs the same sweep against one shared
`Ractor::LockFree::Hash`: `get` and `put`, first with every Ractor cycling over
**1000 keys of its own** and then with all of them on **one hot key**. Each
Ractor cycles over a pre-built array of frozen symbols, so the loop measures the
table and not the cost of making keys; it prints the same loop against a plain
unshared `Hash`, and the loop with no table at all, so the single-Ractor row can
be read for what it is. It also prints `get` with a default beside `get` without
one, which is where you can see that the shareability check on a default is a
flag test and not a traversal.

It finishes by having every Ractor write 20,000 keys of its own and checking that
all of them are still there, so a run that prints `LOST` is a bug and not a slow
machine. These are the numbers quoted in
[docs/hash.md](../docs/hash.md#performance).
