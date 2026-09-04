# frozen_string_literal: true

require_relative "test_helper"

class LockFreeHashTest < Test::Unit::TestCase
  include RactorHelper

  # The way to tell an absent key from a stored nil: a default nothing else
  # can be.
  MISSING = Object.new.freeze

  def setup
    @h = Ractor::LockFree::Hash.new
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
    assert_equal %i[get put],
                 Ractor::LockFree::Hash.instance_methods(false).sort
  end

  def test_get_takes_a_key_and_an_optional_default
    assert_raise(ArgumentError) { @h.get }
    assert_raise(ArgumentError) { @h.get(:k, nil, nil) }
  end

  def test_new_takes_no_arguments
    assert_raise(ArgumentError) { Ractor::LockFree::Hash.new(1) }
    assert_raise(ArgumentError) { Ractor::LockFree::Hash.new({}) }
  end

  def test_the_hash_is_frozen_and_shareable
    assert_true @h.frozen?
    assert_true Ractor.shareable?(@h)
  end

  def test_an_allocated_but_uninitialized_hash_raises
    h = Ractor::LockFree::Hash.allocate
    assert_raise(TypeError) { h.get("k") }
    assert_raise(TypeError) { h.put("k", "v") }
  end

  def test_initialize_cannot_empty_a_live_hash
    @h.put(:k, 1)
    assert_raise(FrozenError) { @h.send(:initialize) }
    assert_equal 1, @h.get(:k)
  end

  def test_subclasses_work
    klass = Class.new(Ractor::LockFree::Hash)
    h = klass.new
    assert_equal 1, h.put(:k, 1)
    assert_equal 1, h.get(:k)
    assert_true Ractor.shareable?(h)
  end

  # --- get and put -------------------------------------------------------

  def test_a_missing_key_reads_as_nil
    assert_nil @h.get(:nope)
  end

  def test_put_returns_the_value
    assert_equal "v", @h.put(:k, "v")
  end

  def test_put_then_get
    @h.put(:k, "v")
    assert_equal "v", @h.get(:k)
  end

  def test_put_replaces_what_was_there
    @h.put(:k, 1)
    @h.put(:k, 2)
    assert_equal 2, @h.get(:k)
    assert_equal 2, @h.get(:k, MISSING)
  end

  def test_the_default_is_returned_only_for_a_key_that_is_not_there
    assert_same MISSING, @h.get(:k, MISSING)
    @h.put(:k, 1)
    assert_equal 1, @h.get(:k, MISSING)
  end

  def test_the_default_defaults_to_nil
    assert_nil @h.get(:k)
    assert_nil @h.get(:k, nil)
  end

  def test_any_shareable_object_will_do_as_a_default
    assert_equal 0, @h.get(:k, 0)
    assert_false @h.get(:k, false)
    assert_equal({}, @h.get(:k, {}.freeze))
    assert_same MISSING, @h.get(:k, MISSING)
  end

  def test_the_default_must_be_shareable
    assert_raise(ArgumentError) { @h.get(:k, +"mutable") }
    assert_raise(ArgumentError) { @h.get(:k, []) }
    assert_raise(ArgumentError) { @h.get(:k, [+"a"].freeze) }
  end

  def test_the_default_is_checked_even_when_the_key_is_there
    @h.put(:k, 1)
    assert_raise(ArgumentError,
                 "or whether a call raises would depend on what is in the table") do
      @h.get(:k, +"mutable")
    end
  end

  def test_the_error_names_the_default
    assert_raise_message(/as a default/) { @h.get(:k, +"mutable") }
  end

  def test_a_stored_nil_is_not_a_missing_key
    @h.put(:k, nil)
    assert_nil @h.get(:k)
    assert_same MISSING, @h.get(:other, MISSING)
    assert_nil @h.get(:k, MISSING), "a stored nil is not the default"
  end

  def test_false_and_true_are_ordinary_values
    @h.put(:f, false)
    @h.put(:t, true)
    assert_false @h.get(:f, MISSING)
    assert_true @h.get(:t, MISSING)
  end

  def test_nil_false_and_true_are_ordinary_keys
    @h.put(nil, :was_nil)
    @h.put(false, :was_false)
    @h.put(true, :was_true)
    assert_equal :was_nil, @h.get(nil)
    assert_equal :was_false, @h.get(false)
    assert_equal :was_true, @h.get(true)
    assert_same MISSING, @h.get(0, MISSING), "and none of them is any other key"
  end

  def test_many_kinds_of_shareable_key
    keys = [:sym, 42, -1, 1 << 70, 2.5, "str".freeze, nil, true,
            [1, :two].freeze, { a: 1 }.freeze, (1..2), Object]
    keys.each_with_index { |k, i| @h.put(k, i) }
    keys.each_with_index { |k, i| assert_equal i, @h.get(k), k.inspect }
  end

  # --- what makes two keys the same ----------------------------------------

  def test_keys_match_by_eql_not_identity
    a = "same".dup.freeze
    b = "same".dup.freeze
    assert_not_same a, b

    @h.put(a, 1)
    assert_equal 1, @h.get(b, MISSING)
    @h.put(b, 2)
    assert_equal 2, @h.get(a), "the second put replaced the first key's value"
  end

  def test_keys_are_told_apart_the_way_hash_does
    @h.put(1, :int)
    @h.put(1.0, :float)
    assert_equal :int, @h.get(1), "1.eql?(1.0) is false, as in a Hash"
    assert_equal :float, @h.get(1.0)
  end

  # --- what may be stored ---------------------------------------------------

  def test_only_shareable_keys
    assert_raise(ArgumentError) { @h.put(+"mutable", 1) }
    assert_raise(ArgumentError) { @h.put([], 1) }
    assert_raise(ArgumentError) { @h.get(+"mutable") }
    assert_raise(ArgumentError) { @h.get(+"mutable", MISSING) }
  end

  def test_only_shareable_values
    assert_raise(ArgumentError) { @h.put(:k, +"mutable") }
    assert_raise(ArgumentError) { @h.put(:k, []) }
  end

  def test_a_frozen_container_of_unshareable_things_is_still_refused
    assert_raise(ArgumentError) { @h.put(:k, [+"a"].freeze) }
    assert_raise(ArgumentError) { @h.put([+"a"].freeze, 1) }
  end

  def test_a_rejected_write_leaves_the_table_alone
    @h.put(:k, 1)
    assert_raise(ArgumentError) { @h.put(:k, +"mutable") }
    assert_equal 1, @h.get(:k)
    assert_same MISSING, @h.get(:fresh, MISSING)
    assert_raise(ArgumentError) { @h.put(:fresh, +"mutable") }
    assert_same MISSING, @h.get(:fresh, MISSING), "no key was claimed either"
  end

  def test_the_error_says_which_argument_was_wrong
    assert_raise_message(/as a key/) { @h.put(+"k", 1) }
    assert_raise_message(/as a value/) { @h.put(:k, +"v") }
  end

  # --- growing --------------------------------------------------------------

  def test_many_keys_survive_the_resizes
    n = 20_000
    n.times { |i| @h.put(i, (i * 2).to_s.freeze) }
    wrong = (0...n).reject { |i| @h.get(i) == (i * 2).to_s }
    assert_equal [], wrong
    assert_same MISSING, @h.get(n, MISSING)
  end

  def test_a_key_written_before_a_resize_is_still_there_after_one
    @h.put(:early, :yes)
    5_000.times { |i| @h.put(:"filler#{i}", i) }
    assert_equal :yes, @h.get(:early)
    @h.put(:early, :updated)
    assert_equal :updated, @h.get(:early)
  end

  def test_reading_finishes_a_migration_that_writing_stopped_half_way
    5_000.times { |i| @h.put(i, i) }
    # Reads alone have to carry the rest, or every later lookup walks two
    # tables for the life of the hash.
    3.times { 5_000.times { |i| assert_equal i, @h.get(i) } }
  end

  def test_survives_gc_and_compaction
    2_000.times { |i| @h.put("k#{i}".freeze, "v#{i}".freeze) }
    GC.start
    GC.compact
    GC.start
    2_000.times { |i| assert_equal "v#{i}", @h.get("k#{i}".freeze) }
    @h.put(:after, :ok)
    assert_equal :ok, @h.get(:after)
  end

  def test_survives_gc_stress
    h = Ractor::LockFree::Hash.new
    begin
      GC.stress = true
      200.times { |i| h.put("s#{i}".freeze, "t#{i}".freeze) }
    ensure
      GC.stress = false
    end
    200.times { |i| assert_equal "t#{i}", h.get("s#{i}".freeze) }
  end

  # --- across Ractors -------------------------------------------------------

  def test_a_ractor_reads_what_the_main_ractor_put
    @h.put(:k, "v")
    assert_equal "v", in_ractor(@h) { |h| h.get(:k) }
  end

  def test_the_main_ractor_reads_what_a_ractor_put
    Ractor.new(@h) { |h| h.put(:k, "v") }.join
    assert_equal "v", @h.get(:k, MISSING)
  end

  def test_a_ractor_sees_the_same_refusals
    quietly do
      assert_raise(ArgumentError) { in_ractor(@h) { |h| h.put(:k, +"mutable") } }
    end
    assert_same MISSING, @h.get(:k, MISSING)
  end

  def test_disjoint_writers_from_many_ractors_lose_nothing
    ractors, per = 4, 2_000
    ractors.times.map { |r|
      Ractor.new(@h, r, per) do |h, rid, n|
        n.times { |i| h.put(:"k#{rid}_#{i}", "#{rid}:#{i}".freeze) }
      end
    }.each(&:join)

    missing = []
    ractors.times do |r|
      per.times do |i|
        got = @h.get(:"k#{r}_#{i}")
        missing << [r, i, got] unless got == "#{r}:#{i}"
      end
    end
    assert_equal [], missing
  end

  def test_racing_writers_on_one_key_leave_one_of_the_written_values
    ractors = 4
    ractors.times.map { |r|
      Ractor.new(@h, r) do |h, rid|
        5_000.times { 5.times { |k| h.put(:"hot#{k}", rid) } }
      end
    }.each(&:join)

    5.times { |k| assert_include((0...ractors).to_a, @h.get(:"hot#{k}")) }
  end

  def test_a_reader_never_sees_a_wrong_value_while_the_table_grows
    writers = 4.times.map { |r|
      Ractor.new(@h, r) do |h, rid|
        2_000.times { |i| h.put(:"k#{rid}_#{i}", rid * 1_000_000 + i) }
      end
    }
    readers = 2.times.map {
      Ractor.new(@h) do |h|
        wrong = seen = 0
        50_000.times do |n|
          rid, i = n % 4, n % 2_000
          v = h.get(:"k#{rid}_#{i}", :missing)
          next if v == :missing
          seen += 1
          wrong += 1 unless v == rid * 1_000_000 + i
        end
        [seen, wrong]
      end
    }
    writers.each(&:join)
    readers.map(&:value).each_with_index do |(seen, wrong), i|
      assert_equal 0, wrong, "reader #{i} saw a value that was never written"
      assert_operator seen, :>, 0, "reader #{i} never saw anything at all"
    end
  end

  def test_a_value_never_goes_backwards_for_a_reader
    keys, rounds = 100, 60
    writers = 4.times.map { |r|
      Ractor.new(@h, r, keys, rounds) do |h, rid, nkeys, nrounds|
        nrounds.times { |round| nkeys.times { |k| h.put(:"m#{rid}_#{k}", round) } }
      end
    }
    watchers = 2.times.map {
      Ractor.new(@h) do |h|
        last = {}
        back = 0
        50_000.times do |n|
          key = :"m#{n % 4}_#{n % 100}"
          v = h.get(key)
          next if v.nil?
          back += 1 if last[key] && v < last[key]
          last[key] = v
        end
        back
      end
    }
    writers.each(&:join)
    watchers.map(&:value).each_with_index do |back, i|
      assert_equal 0, back, "watcher #{i} saw an update undone"
    end
    keys.times do |k|
      4.times { |r| assert_equal rounds - 1, @h.get(:"m#{r}_#{k}") }
    end
  end

  def test_gc_from_one_ractor_while_others_grow_the_table
    gc = Ractor.new { 200.times { GC.start } }
    writers = 4.times.map { |r|
      Ractor.new(@h, r) do |h, rid|
        1_500.times { |i| h.put("k#{rid}-#{i}".freeze, "v#{rid}-#{i}".freeze) }
      end
    }
    readers = 2.times.map {
      Ractor.new(@h) do |h|
        wrong = 0
        20_000.times do |n|
          rid, i = n % 4, n % 1_500
          v = h.get("k#{rid}-#{i}".freeze, :missing)
          wrong += 1 if v != :missing && v != "v#{rid}-#{i}"
        end
        wrong
      end
    }
    writers.each(&:join)
    gc.join
    readers.map(&:value).each_with_index { |w, i| assert_equal 0, w, "reader #{i}" }
    4.times do |r|
      1_500.times { |i| assert_equal "v#{r}-#{i}", @h.get("k#{r}-#{i}".freeze) }
    end
  end
end
