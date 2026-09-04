# frozen_string_literal: true

require_relative "test_helper"

class LockFreeVarTest < Test::Unit::TestCase
  include RactorHelper

  def setup
    @var = Ractor::LockFree::Var.new(String, "hi")
  end

  # A Ractor that raises reports it on stderr before its owner sees it.
  def quietly
    was = Thread.report_on_exception
    Thread.report_on_exception = false
    yield
  ensure
    Thread.report_on_exception = was
  end

  # --- API ----------------------------------------------------------------

  def test_public_api
    assert_equal %i[compare_and_swap get inspect set swap type],
                 Ractor::LockFree::Var.instance_methods(false).sort
  end

  def test_new_requires_a_type_and_a_value
    assert_raise(ArgumentError) { Ractor::LockFree::Var.new }
    assert_raise(ArgumentError) { Ractor::LockFree::Var.new(String) }
    assert_raise(ArgumentError) { Ractor::LockFree::Var.new(String, "a", "b") }
  end

  def test_type
    assert_equal String, @var.type
    assert_equal Integer, Ractor::LockFree::Var.new(Integer, 0).type
  end

  def test_get
    assert_equal "hi", @var.get
  end

  def test_set_returns_the_value
    assert_equal "omg", @var.set("omg")
    assert_equal "omg", @var.get
  end

  def test_inspect
    assert_equal '#<Ractor::LockFree::Var String "hi">', @var.inspect
    assert_equal "#<Ractor::LockFree::Var Integer 0>", Ractor::LockFree::Var.new(Integer, 0).inspect
  end

  def test_the_variable_is_frozen_and_shareable
    assert_true @var.frozen?
    assert_true Ractor.shareable?(@var)
  end

  def test_a_module_is_a_type_too
    var = Ractor::LockFree::Var.new(Comparable, 1)
    assert_equal 1, var.get
    assert_equal :ok, var.set(:ok)
    assert_raise(TypeError) { var.set(nil) }
  end

  def test_object_accepts_anything_shareable_including_nil
    var = Ractor::LockFree::Var.new(Object, nil)
    assert_nil var.get
    assert_equal 1, var.set(1)
    assert_equal "s", var.set("s")
  end

  def test_a_type_must_be_a_class_or_module
    assert_raise(TypeError) { Ractor::LockFree::Var.new("String", "x") }
    assert_raise(TypeError) { Ractor::LockFree::Var.new(nil, "x") }
    assert_raise(TypeError) { Ractor::LockFree::Var.new(:String, "x") }
  end

  def test_subclasses_work
    klass = Class.new(Ractor::LockFree::Var)
    var = klass.new(Integer, 7)
    assert_equal 7, var.get
    assert_true var.compare_and_swap(7, 8)
    assert_equal 8, var.get
  end

  def test_an_allocated_but_uninitialized_variable_raises
    var = Ractor::LockFree::Var.allocate
    assert_equal "#<Ractor::LockFree::Var (uninitialized)>", var.inspect
    assert_raise(TypeError) { var.get }
    assert_raise(TypeError) { var.set("x") }
    assert_raise(TypeError) { var.type }
  end

  def test_initialize_cannot_retype_a_live_variable
    assert_raise(FrozenError) { @var.send(:initialize, Integer, 1) }
    assert_equal "hi", @var.get
  end

  # --- what may be stored --------------------------------------------------

  def test_the_type_is_checked_on_every_write
    assert_raise(TypeError) { Ractor::LockFree::Var.new(String, 1) }
    assert_raise(TypeError) { @var.set(1) }
    assert_raise(TypeError) { @var.swap(1) }
    assert_raise(TypeError) { @var.compare_and_swap("hi", 1) }
    assert_equal "hi", @var.get, "a rejected write leaves the value alone"
  end

  def test_only_shareable_values
    assert_raise(ArgumentError) { Ractor::LockFree::Var.new(Array, []) }
    assert_raise(ArgumentError) { Ractor::LockFree::Var.new(String, +"mutable") }

    var = Ractor::LockFree::Var.new(Array, [].freeze)
    assert_raise(ArgumentError) { var.set([1, 2]) }
    assert_raise(ArgumentError) { var.swap([1, 2]) }
    assert_raise(ArgumentError) { var.compare_and_swap([], [1, 2]) }
    assert_equal [], var.get, "a rejected write leaves the value alone"

    assert_equal [1, 2], var.set([1, 2].freeze)
  end

  def test_a_frozen_container_of_unshareable_things_is_still_refused
    assert_raise(ArgumentError) { Ractor::LockFree::Var.new(Array, [+"a"].freeze) }
  end

  # --- swap ----------------------------------------------------------------

  def test_swap_returns_the_previous_value
    assert_equal "hi", @var.swap("next")
    assert_equal "next", @var.get
  end

  def test_every_swapper_gets_a_different_previous_value
    var = Ractor::LockFree::Var.new(Integer, 0)
    seen = Queue.new
    threads = 8.times.map { |i| Thread.new { 50.times { |j| seen << var.swap(i * 50 + j + 1) } } }
    threads.each(&:join)
    got = Array.new(seen.size) { seen.pop } << var.get
    assert_equal (0..400).to_a, got.sort,
                 "each value went in once and came back out exactly once"
  end

  # --- compare_and_swap -----------------------------------------------------

  def test_compare_and_swap
    assert_true @var.compare_and_swap("hi", "yes")
    assert_equal "yes", @var.get
    assert_false @var.compare_and_swap("hi", "no")
    assert_equal "yes", @var.get
  end

  def test_compare_and_swap_compares_by_identity_not_equality
    a = "same".dup.freeze
    b = "same".dup.freeze
    assert_equal a, b
    assert_not_same a, b

    var = Ractor::LockFree::Var.new(String, a)
    assert_false var.compare_and_swap(b, "x"), "an equal but different object is not a match"
    assert_true var.compare_and_swap(a, "x")
  end

  def test_exactly_one_of_many_racing_compare_and_swaps_wins
    var = Ractor::LockFree::Var.new(Integer, 0)
    start = Queue.new
    threads = 8.times.map do |i|
      Thread.new { start.pop; var.compare_and_swap(0, i + 1) }
    end
    8.times { start << :go }
    assert_equal 1, threads.map(&:value).count(true)
  end

  # --- compare_and_swap with a block ----------------------------------------

  def test_a_block_decides_the_match
    a = "same".dup.freeze
    b = "same".dup.freeze
    var = Ractor::LockFree::Var.new(String, a)

    assert_false var.compare_and_swap(b, "x".freeze), "identity is still the default"
    assert_true var.compare_and_swap(b, "x".freeze) { |cur, exp| cur == exp }
    assert_equal "x", var.get
  end

  def test_a_block_is_handed_the_current_value_then_the_expected_one
    var = Ractor::LockFree::Var.new(Integer, 7)
    seen = nil
    var.compare_and_swap(9, 8) { |cur, exp| seen = [cur, exp]; true }
    assert_equal [7, 9], seen
  end

  def test_a_falsy_block_writes_nothing
    assert_false @var.compare_and_swap("hi", "no") { false }
    assert_equal "hi", @var.get
    assert_false @var.compare_and_swap("hi", "no") { nil }
    assert_equal "hi", @var.get
  end

  def test_a_block_runs_exactly_once
    runs = 0
    assert_true @var.compare_and_swap("hi", "yes") { runs += 1; true }
    assert_equal 1, runs
  end

  # The point of the block form: the block says what counts as a match, but the
  # store is still conditional on the exact object it was shown.
  def test_a_write_during_the_block_defeats_the_compare_and_swap
    var = Ractor::LockFree::Var.new(Integer, 1)
    result = var.compare_and_swap(1, 2) { |cur, exp| var.set(100); cur == exp }
    assert_false result, "the object the block judged is no longer there"
    assert_equal 100, var.get
  end

  def test_a_block_that_raises_leaves_the_variable_alone
    assert_raise(RuntimeError) { @var.compare_and_swap("hi", "no") { raise "nope" } }
    assert_equal "hi", @var.get
  end

  # The type check comes first, so a call that could never have stored anything
  # does not get to run a block with a side effect in it.
  def test_a_rejected_value_never_runs_the_block
    ran = false
    assert_raise(TypeError) { @var.compare_and_swap("hi", 1) { ran = true } }
    assert_raise(ArgumentError) { @var.compare_and_swap("hi", +"unshareable") { ran = true } }
    assert_false ran
    assert_equal "hi", @var.get
  end

  def test_exactly_one_of_many_racing_blocks_wins
    var = Ractor::LockFree::Var.new(Integer, 0)
    start = Queue.new
    threads = 8.times.map do |i|
      Thread.new { start.pop; var.compare_and_swap(0, i + 1) { |cur, exp| cur == exp } }
    end
    8.times { start << :go }
    assert_equal 1, threads.map(&:value).count(true)
  end

  def test_a_block_works_across_ractors
    var = Ractor::LockFree::Var.new(Hash, { seq: 0 }.freeze)
    4.times.map do
      Ractor.new(var) do |v|
        250.times do
          loop do
            old = v.get
            break if v.compare_and_swap(old, { seq: old[:seq] + 1 }.freeze) { |c, e| c == e }
          end
        end
      end
    end.each(&:join)
    assert_equal 1000, var.get[:seq]
  end

  # --- read-modify-write, by hand ------------------------------------------

  # There is no #update: a read-modify-write is a compare_and_swap loop, and the
  # loop is the caller's so the caller decides how long to keep trying.
  def increment(var)
    loop do
      old = var.get
      return old + 1 if var.compare_and_swap(old, old + 1)
    end
  end

  def test_a_retry_loop_loses_no_writes_across_threads
    var = Ractor::LockFree::Var.new(Integer, 0)
    8.times.map { Thread.new { 500.times { increment(var) } } }.each(&:join)
    assert_equal 4000, var.get
  end

  def test_a_retry_loop_loses_no_writes_across_ractors
    var = Ractor::LockFree::Var.new(Integer, 0)
    rs = 4.times.map do
      Ractor.new(var) do |v|
        500.times { loop { old = v.get; break if v.compare_and_swap(old, old + 1) } }
      end
    end
    rs.each(&:join)
    assert_equal 2000, var.get
  end

  # Writing between the get and the compare_and_swap is the one way to lose on
  # purpose, and losing must cost the loser its write and nothing else.
  def test_a_write_that_lands_in_between_defeats_the_compare_and_swap
    var = Ractor::LockFree::Var.new(Integer, 0)

    old = var.get
    var.set(100)

    assert_false var.compare_and_swap(old, old + 1)
    assert_equal 100, var.get, "the losing attempt stored nothing"
    assert_true var.compare_and_swap(100, 101), "and the loser can just try again"
  end

  # An attempt is one atomic instruction with nothing held, so there is no state
  # a kill between attempts could strand.
  def test_killing_a_thread_mid_retry_leaves_the_variable_usable
    var = Ractor::LockFree::Var.new(Integer, 1)
    inside = Queue.new
    t = Thread.new do
      loop do
        old = var.get
        inside << :in
        sleep 5   # a killed attempt, between the read and the write
        break if var.compare_and_swap(old, old + 1)
      end
    end
    inside.pop
    t.kill
    t.join(3)

    assert_equal 1, var.get
    assert_true var.compare_and_swap(1, 2)
  end

  # No lock means no lock ordering, so unlike Ractor::LockVar there is nothing to
  # refuse: any variable may be touched between any other's read and write.
  def test_there_is_no_lock_order_between_variables
    a = Ractor::LockFree::Var.new(Integer, 1)
    b = Ractor::LockFree::Var.new(Integer, 10)

    old = a.get
    assert_true a.compare_and_swap(old, old + b.get)
    assert_equal 11, a.get
  end

  # --- across Ractors ------------------------------------------------------

  def test_a_ractor_reads_what_the_main_ractor_wrote
    @var.set("written before the Ractor started")
    assert_equal "written before the Ractor started", in_ractor(@var) { |v| v.get }
  end

  def test_the_main_ractor_reads_what_a_ractor_wrote
    Ractor.new(@var) { |v| v.set("wonderful") }.join
    assert_equal "wonderful", @var.get
  end

  def test_a_write_is_visible_to_every_other_ractor
    var = Ractor::LockFree::Var.new(Integer, 0)
    stop = Ractor::LockFree::Var.new(Object, false)

    # Each reader watches for the value to reach 100. A read that could go stale
    # for good would hang here rather than return.
    readers = 4.times.map do
      Ractor.new(var, stop) { |v, s| Thread.pass until v.get == 100 || s.get; v.get }
    end
    Thread.new { 1.upto(100) { |i| var.set(i) } }.join

    assert_equal [100] * 4, readers.map(&:value)
  ensure
    stop&.set(true)
  end

  def test_the_type_is_enforced_from_another_ractor_too
    # The Ractor is meant to die of it; its report would just be noise.
    quietly { assert_raise(TypeError) { in_ractor(@var) { |v| v.set(1) } } }
    assert_equal "hi", @var.get
  end

  def test_reads_and_writes_from_many_ractors_never_tear
    # Every value is a freshly allocated frozen string, so each write makes the
    # last one garbage: a reader that saw a half-published or collected slot
    # would see something that is not one of the strings written.
    var = Ractor::LockFree::Var.new(String, "s")
    stop = Ractor::LockFree::Var.new(Object, false)

    writers = 3.times.map do |i|
      Ractor.new(var, stop, i) do |v, s, n|
        c = 0
        until s.get
          v.set("w#{n}-#{"x" * (c % 100)}".freeze)
          GC.start(full_mark: false) if (c % 2000).zero?
          c += 1
        end
        c
      end
    end
    readers = 3.times.map do
      Ractor.new(var, stop) do |v, s|
        until s.get
          x = v.get
          raise "corrupt: #{x.inspect[0, 40]}" unless x.is_a?(String) && x.frozen?
          raise "empty" if x.empty?
        end
        :ok
      end
    end

    sleep 1
    stop.set(true)
    assert_operator writers.map(&:value).sum, :>, 0
    assert_equal %i[ok ok ok], readers.map(&:value)
  end
end
