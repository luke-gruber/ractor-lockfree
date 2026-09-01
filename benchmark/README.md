# Benchmarks

```
ruby benchmark/shared_var/scaling.rb [max_ractors]
```

`shared_var/scaling.rb` runs the same workload — one shared frozen
`{status:, seq:}` record — across 1, 2, 4, 8 and 16 Ractors, first with every
Ractor on **one shared variable** and then with **one variable each**, and
reports nanoseconds per completed operation across all Ractors. A number that
halves when the Ractors double means the operation scaled.

It finishes by checking that a contended `compare_and_swap` retry loop lost no
writes, so a run that prints `LOST WRITES` is a bug and not a slow machine.

If [ractor-sharing](https://github.com/ko1/ractor-sharing) is installed it also
prints the same workload against the locking `Ractor::LockVar`, which is the
comparison quoted in [docs/shared_var.md](../docs/shared_var.md#performance).
