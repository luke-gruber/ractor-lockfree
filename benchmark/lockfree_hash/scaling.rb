# frozen_string_literal: true

# How Ractor::LockFree::Hash scales: N Ractors doing the same operation on one
# shared table, first on a set of keys each and then all on one hot key.
#
#   ruby benchmark/lockfree_hash/scaling.rb [max_ractors]
#
# Reported as nanoseconds per completed operation across all Ractors, so a
# number that halves when the Ractors double means the operation scaled.
#
# Every Ractor cycles over a pre-built array of frozen keys, so the loop
# measures the table and not the cost of making keys.

Warning[:experimental] = false
$LOAD_PATH.unshift File.expand_path("../../lib", __dir__)
require "ractor/lockfree"

MAX    = (ARGV[0] || 16).to_i
COUNTS = [1, 2, 4, 8, 16, 32].select { |n| n <= MAX }
K      = 200_000   # operations per Ractor
M      = 10_000    # distinct keys per Ractor
RUNS   = 3         # median of

VALUE = { status: :ok, seq: 1 }.freeze
HOT   = :hot

def keys_for(rid) = Array.new(M) { |i| :"k#{rid}_#{i}" }.freeze

# `op` K times, from n Ractors at once, against the one table.
def measure(table, n, op)
  rs = n.times.map do |rid|
    keys = op == :hot_get || op == :hot_put ? [HOT].freeze : keys_for(rid)
    Ractor.new(table, keys, op, K, VALUE) do |h, ks, o, k, v|
      m = ks.size
      case o
      when :get, :hot_get then k.times { |i| h.get(ks[i % m]) }
      when :get_default   then k.times { |i| h.get(ks[i % m], :missing) }
      when :put, :hot_put then k.times { |i| h.put(ks[i % m], v) }
      end
      :done
    end
  end
  t = Process.clock_gettime(Process::CLOCK_MONOTONIC)
  rs.each(&:join)
  Process.clock_gettime(Process::CLOCK_MONOTONIC) - t
end

# A read benchmark has to read something, so the table is filled first -- and
# filled for every Ractor the sweep will use, not just this run's.
def filled(n)
  h = Ractor::LockFree::Hash.new
  h.put(HOT, VALUE)
  n.times { |rid| keys_for(rid).each { |k| h.put(k, VALUE) } }
  h
end

READS = %i[get hot_get get_default].freeze

def bench(n, op)
  times = RUNS.times.map do
    table = READS.include?(op) ? filled(n) : filled(0)
    measure(table, n, op)
  end
  times.sort[RUNS / 2] * 1e9 / (n * K)
end

puts "ruby #{RUBY_VERSION} (#{RbConfig::CONFIG["host_cpu"]}), #{K} ops per Ractor, median of #{RUNS}"
puts

puts "| Ractors | `get`, key each | `get`, one hot key | `put`, key each | `put`, one hot key |"
puts "|---:|---:|---:|---:|---:|"
COUNTS.each do |n|
  printf("| %d | %.0f | %.0f | %.0f | %.0f |\n", n,
         bench(n, :get), bench(n, :hot_get), bench(n, :put), bench(n, :hot_put))
end
puts

# The same loop against a plain unshared Hash, in one Ractor, for scale: what
# the operation costs when nothing is shared and no atomics are involved.
plain = {}
keys = keys_for(0)
keys.each { |k| plain[k] = VALUE }

def timed(k)
  t = Process.clock_gettime(Process::CLOCK_MONOTONIC)
  yield
  (Process.clock_gettime(Process::CLOCK_MONOTONIC) - t) * 1e9 / k
end

loop_only = timed(K) { K.times { |i| keys[i % M] } }
read  = timed(K) { K.times { |i| plain[keys[i % M]] } }
write = timed(K) { K.times { |i| plain[keys[i % M]] = VALUE } }
printf("plain Hash in one Ractor: [] %.0f ns, []= %.0f ns\n", read, write)
printf("the loop itself (no table at all): %.0f ns\n", loop_only)
printf("get with a default, 1 Ractor: %.0f ns (against %.0f without one)\n\n",
       bench(1, :get_default), bench(1, :get))

# Disjoint writers must lose nothing, however many of them there are and however
# many resizes they drive.
h = Ractor::LockFree::Hash.new
n, per = [COUNTS.max, 4].max, 20_000
n.times.map { |rid|
  Ractor.new(h, rid, per) do |hash, r, count|
    count.times { |i| hash.put(:"w#{r}_#{i}", i) }
    :done
  end
}.each(&:join)
lost = n.times.sum { |r| per.times.count { |i| h.get(:"w#{r}_#{i}") != i } }
puts "check: #{n * per} keys written by #{n} Ractors -> #{lost.zero? ? "ok" : "#{lost} LOST"}"
