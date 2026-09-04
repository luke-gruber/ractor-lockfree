# frozen_string_literal: true

# How Ractor::LockFree::Var scales: N Ractors doing the same operation, on one
# shared variable and then on one variable each.
#
#   ruby benchmark/lockfree_var/scaling.rb [max_ractors]
#
# Reported as nanoseconds per completed operation across all Ractors, so a
# number that halves when the Ractors double means the operation scaled.
#
# `cas` is a read-modify-write written the way callers have to write one: a
# compare_and_set retry loop in Ruby, replacing a frozen record with the next.

Warning[:experimental] = false
$LOAD_PATH.unshift File.expand_path("../../lib", __dir__)
require "ractor/lockfree"

MAX     = (ARGV[0] || 8).to_i
COUNTS  = [1, 2, 4, 8, 16].select { |n| n <= MAX }
K       = 200_000   # operations per Ractor
RUNS    = 3         # median of

RECORD = { status: :ok, seq: 0 }.freeze

# Each worker runs `op` K times against the variable it is handed.
def measure(vars, op)
  rs = vars.map do |var|
    Ractor.new(var, op, K) do |v, o, k|
      case o
      when :get then k.times { v.get }
      when :set then rec = { status: :ok, seq: 1 }.freeze; k.times { v.set(rec) }
      when :cas
        k.times do
          loop do
            old = v.get
            break if v.compare_and_set(old, { status: old[:status], seq: old[:seq] + 1 }.freeze)
          end
        end
      end
      :done
    end
  end
  t = Process.clock_gettime(Process::CLOCK_MONOTONIC)
  rs.each(&:join)
  Process.clock_gettime(Process::CLOCK_MONOTONIC) - t
end

def bench(n, op, shared:)
  one = Ractor::LockFree::Var.new(Hash, RECORD)
  times = RUNS.times.map do
    vars = shared ? Array.new(n) { one } : Array.new(n) { Ractor::LockFree::Var.new(Hash, RECORD) }
    measure(vars, op)
  end
  times.sort[RUNS / 2] * 1e9 / (n * K)
end

puts "ruby #{RUBY_VERSION} (#{RbConfig::CONFIG["host_cpu"]}), #{K} ops per Ractor, median of #{RUNS}"
puts

%i[get set cas].each do |op|
  puts "## #{op}"
  puts
  puts "| Ractors | shared (ns) | one each (ns) |"
  puts "|---:|---:|---:|"
  COUNTS.each do |n|
    printf("| %d | %.0f | %.0f |\n", n, bench(n, op, shared: true), bench(n, op, shared: false))
  end
  puts
end

# A retry loop must never lose a write, however many Ractors fight over it.
counter = Ractor::LockFree::Var.new(Integer, 0)
n, k = [COUNTS.max, 4].max, 5_000
n.times.map do
  Ractor.new(counter, k) do |v, m|
    m.times { loop { old = v.get; break if v.compare_and_set(old, old + 1) } }
  end
end.each(&:join)
puts "check: #{counter.get} == #{n * k} -> #{counter.get == n * k ? "ok" : "LOST WRITES"}"

# The same workload against Ractor::LockVar, when ractor-sharing happens to be
# installed: the locking version of the same idea, for scale.
begin
  require "ractor/lockvar"
rescue LoadError
  puts "\n(install ractor-sharing for the Ractor::LockVar comparison)"
else
  def lockvar_bench(n, op)
    one = Ractor::LockVar.new(RECORD)
    times = RUNS.times.map do
      rs = Array.new(n) do
        Ractor.new(one, op, K) do |v, o, k|
          o == :get ? k.times { v.value }
                    : k.times { v.update { |r| { status: r[:status], seq: r[:seq] + 1 }.freeze } }
          :done
        end
      end
      t = Process.clock_gettime(Process::CLOCK_MONOTONIC)
      rs.each(&:join)
      Process.clock_gettime(Process::CLOCK_MONOTONIC) - t
    end
    times.sort[RUNS / 2] * 1e9 / (n * K)
  end

  puts "\n## versus Ractor::LockVar, all Ractors on one variable\n\n"
  puts "| Ractors | LockFree::Var#get | LockVar#value | LockFree::Var cas loop | LockVar#update |"
  puts "|---:|---:|---:|---:|---:|"
  COUNTS.each do |n|
    printf("| %d | %.0f | %.0f | %.0f | %.0f |\n", n,
           bench(n, :get, shared: true), lockvar_bench(n, :get),
           bench(n, :cas, shared: true), lockvar_bench(n, :update))
  end
end
