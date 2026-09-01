#include "ruby/ruby.h"
#include "ruby/atomic.h"
#include "ruby/ractor.h"

/* Ractor::SharedVar - one typed, shareable value, replaced without a lock.
 *
 * The whole variable is a single VALUE-sized slot, and every operation on it is
 * one atomic on that slot: a load for #get, a store for #set, an exchange for
 * #swap, a compare-and-swap for #compare_and_swap.  None of them loops, so there
 * is no mutex, no waiter queue and no retry: no caller can block another one,
 * and a Ractor killed mid-operation leaves nothing to clean up.
 *
 * #compare_and_swap takes an optional block to decide the match with, which is
 * the one place Ruby runs in the middle of an operation.  It still cannot loop
 * or wait -- the block is called exactly once -- and the store it guards is
 * still one compare-and-swap against the very object the block was shown, so a
 * write that lands while the block thinks simply makes the whole call fail.
 *
 * A read-modify-write is #compare_and_swap in a loop, and that loop belongs to
 * the caller, who is the one who knows how many attempts are worth making and
 * what to do instead of another one.
 *
 * See "Ordering" below for what the atomics do and do not promise.
 *
 * The type is fixed when the variable is made and checked on every write, so a
 * reader can rely on what it gets back without any agreement between Ractors
 * about who writes what.  That check, rb_obj_is_kind_of, walks the ancestry in
 * C: it dispatches no Ruby method, so a write cannot be interrupted between the
 * check and the store it guards.
 */

static VALUE rb_cRactorSharedVar;

/* Ordering.
 *
 * The ordering this variable needs is exactly release/acquire, and that is all
 * it asks for:
 *
 *   - a write is a RELEASE store, so everything the writer did to the object
 *     before handing it over -- filling it in, freezing it -- is finished
 *     before any other Ractor can see the pointer to it;
 *   - a read is an ACQUIRE load, so a Ractor that sees the pointer also sees
 *     that finished object, and never a half-built one;
 *   - #swap and #compare_and_swap both consume a value and publish one, so they
 *     are ACQ_REL, with a plain ACQUIRE on the compare-and-swap's failure path,
 *     which publishes nothing.
 *
 * That pairing is the whole safety argument for passing an object between
 * Ractors through this slot, and it is weaker than sequential consistency on
 * purpose.  What SEQ_CST would add is a single global order over operations on
 * *different* SharedVars -- something this library has never promised, that no
 * documented behaviour rests on, and that costs a full barrier on every write.
 *
 * What is kept is everything that is promised.  Per-variable ordering does not
 * come from SEQ_CST at all: every atomic object has one modification order that
 * all threads agree on whatever ordering its operations use, so two writes to
 * one SharedVar can never be seen in opposite orders by two Ractors.  And a
 * read-modify-write always acts on the latest value in that order, which is
 * what makes #swap and #compare_and_swap decisive rather than advisory.
 *
 * Ruby's public RUBY_ATOMIC_* macros hardcode SEQ_CST, but the ordering is a
 * parameter one layer down, on the rbimpl_atomic_* inline functions the same
 * public header ships.  Those are the only internal names this file uses, so it
 * uses them behind a fallback: where they are absent these become the public
 * SEQ_CST macros, which are strictly stronger and so still correct -- only
 * slower, and only on hardware that charges for the difference. */
#if defined(RBIMPL_ATOMIC_ACQUIRE) && defined(RBIMPL_ATOMIC_RELEASE) && defined(RBIMPL_ATOMIC_ACQ_REL)
# define SV_LOAD_ACQUIRE(var)          rbimpl_atomic_value_load(&(var), RBIMPL_ATOMIC_ACQUIRE)
# define SV_STORE_RELEASE(var, val)    rbimpl_atomic_value_store(&(var), (val), RBIMPL_ATOMIC_RELEASE)
# define SV_EXCHANGE_ACQ_REL(var, val) rbimpl_atomic_value_exchange(&(var), (val), RBIMPL_ATOMIC_ACQ_REL)
# define SV_CAS_ACQ_REL(var, old, new) \
    rbimpl_atomic_value_cas(&(var), (old), (new), RBIMPL_ATOMIC_ACQ_REL, RBIMPL_ATOMIC_ACQUIRE)
#else
# define SV_LOAD_ACQUIRE(var)          ((VALUE)RUBY_ATOMIC_PTR_LOAD(var))
# define SV_STORE_RELEASE(var, val)    RUBY_ATOMIC_VALUE_SET((var), (val))
# define SV_EXCHANGE_ACQ_REL(var, val) RUBY_ATOMIC_VALUE_EXCHANGE((var), (val))
# define SV_CAS_ACQ_REL(var, old, new) RUBY_ATOMIC_VALUE_CAS((var), (old), (new))
#endif

struct shared_var {
    VALUE value;   /* only ever touched through the SV_* atomics above */
    VALUE type;    /* a Module; set once by #initialize and never written again.
                    * Read plainly: it is written before the variable is frozen
                    * and made shareable, and whatever handed the variable to
                    * another Ractor is what orders that write against the read. */
};

static void
shared_var_mark(void *ptr)
{
    struct shared_var *sv = ptr;

    /* GC marking runs with every Ractor stopped, so a plain read would do; the
     * acquire load costs nothing here and keeps the rule -- this slot is only
     * ever read with an acquire -- true of every line that touches it. */
    rb_gc_mark(SV_LOAD_ACQUIRE(sv->value));
    rb_gc_mark(sv->type);
}

static size_t
shared_var_memsize(const void *ptr)
{
    return sizeof(struct shared_var);
}

/* Deliberately not RUBY_TYPED_WB_PROTECTED. Doesn't matter on Ruby 4.1.0 (rlgc) because it's
 * marked shareable anyway.
 */
static const rb_data_type_t shared_var_data_type = {
    "Ractor::SharedVar",
    {shared_var_mark, RUBY_TYPED_DEFAULT_FREE, shared_var_memsize, NULL},
    0, 0, RUBY_TYPED_FREE_IMMEDIATELY | RUBY_TYPED_FROZEN_SHAREABLE
};

static struct shared_var *
shared_var_ptr(VALUE self)
{
    struct shared_var *sv;

    TypedData_Get_Struct(self, struct shared_var, &shared_var_data_type, sv);
    /* .allocate without .new leaves no type, and every write is checked against
     * the type. */
    if (RB_UNLIKELY(NIL_P(sv->type))) {
        rb_raise(rb_eTypeError, "uninitialized %"PRIsVALUE, rb_obj_class(self));
    }
    return sv;
}

/* Everything a value must satisfy to go into the slot.  Raises, and raising
 * before any atomic runs is what leaves a rejected write with no effect.
 * Takes the type rather than reading it off the variable so that #initialize can
 * check a value against a type it has not committed to yet. */
static void
shared_var_check(VALUE type, VALUE val)
{
    if (RB_UNLIKELY(!RTEST(rb_obj_is_kind_of(val, type)))) {
        rb_raise(rb_eTypeError, "expected a %"PRIsVALUE", got %"PRIsVALUE,
                 type, rb_obj_class(val));
    }
    if (RB_UNLIKELY(!rb_ractor_shareable_p(val))) {
        rb_raise(rb_eArgError, "only shareable objects are allowed, got an unshareable %"PRIsVALUE,
                 rb_obj_class(val));
    }
}

static VALUE
shared_var_alloc(VALUE klass)
{
    struct shared_var *sv;
    VALUE obj = TypedData_Make_Struct(klass, struct shared_var, &shared_var_data_type, sv);

    sv->value = Qnil;
    sv->type = Qnil;
    return obj;
}

/*
 *  call-seq:
 *     Ractor::SharedVar.new(type, value) -> shared_var
 *
 *  Makes a variable holding +value+, which every later write must be a +type+.
 */
static VALUE
shared_var_initialize(VALUE self, VALUE type, VALUE value)
{
    struct shared_var *sv;

    /* Without this, send(:initialize) on a variable already in use would
     * retype it under whoever was reading it. */
    rb_check_frozen(self);
    TypedData_Get_Struct(self, struct shared_var, &shared_var_data_type, sv);

    if (RB_UNLIKELY(!RTEST(rb_obj_is_kind_of(type, rb_cModule)))) {
        rb_raise(rb_eTypeError, "type must be a Class or Module, got %"PRIsVALUE,
                 rb_obj_class(type));
    }
    /* The type goes in only once the value has passed against it: a rejected
     * #initialize leaves an untyped variable, which every method refuses, rather
     * than a typed one holding nil. */
    shared_var_check(type, value);
    sv->type = type;
    /* Nothing can reach this variable yet, so ordering is not in question; the
     * release keeps every store to the slot the same shape as every other. */
    SV_STORE_RELEASE(sv->value, value);

    /* Nothing in the object itself changes after this, so it can be frozen and
     * handed to any Ractor; RUBY_TYPED_FROZEN_SHAREABLE is what allows it for a
     * T_DATA.  The slot inside is not part of the object's Ruby-visible state. */
    rb_obj_freeze(self);
    rb_ractor_make_shareable(self);
    return self;
}

/*
 *  call-seq:
 *     shared_var.get -> value
 *
 *  The value written by the most recently completed write, from any Ractor.
 *  Never blocks and never raises.
 */
static VALUE
shared_var_get(VALUE self)
{
    return SV_LOAD_ACQUIRE(shared_var_ptr(self)->value);
}

/*
 *  call-seq:
 *     shared_var.set(value) -> value
 *
 *  Makes +value+ what every later #get returns, and returns it.  Never blocks.
 *  Raises TypeError unless +value+ is of the variable's type, and ArgumentError
 *  unless it is shareable; either way the variable is left as it was.
 */
static VALUE
shared_var_set(VALUE self, VALUE value)
{
    struct shared_var *sv = shared_var_ptr(self);

    shared_var_check(sv->type, value);
    SV_STORE_RELEASE(sv->value, value);
    return value;
}

/*
 *  call-seq:
 *     shared_var.swap(value) -> previous value
 *
 *  Sets +value+ and returns what was there, in one step, so the value it returns
 *  is one no other caller can also be handed.
 */
static VALUE
shared_var_swap(VALUE self, VALUE value)
{
    struct shared_var *sv = shared_var_ptr(self);

    shared_var_check(sv->type, value);
    return SV_EXCHANGE_ACQ_REL(sv->value, value);
}

/*
 *  call-seq:
 *     shared_var.compare_and_swap(expected, value) -> true or false
 *     shared_var.compare_and_swap(expected, value) {|current, expected| bool } -> true or false
 *
 *  Sets +value+ only if the variable still matches +expected+, and says whether
 *  it did.
 *
 *  Without a block the match is identity, as #equal? does and not ==, and the
 *  call is the single compare-and-swap the hardware provides.
 *
 *  With a block the match is whatever the block says.  It is handed the value
 *  that is there and +expected+, in that order, and is called exactly once; a
 *  true return lets the write go ahead.  The write is still conditional on the
 *  variable holding the very object the block was shown, so a write from another
 *  Ractor that lands while the block runs makes this return false -- even if the
 *  value it wrote would also have matched.  That is what keeps the block's
 *  verdict true of the object actually replaced.
 *
 *  +value+ is checked against the type before the block runs, so a call that
 *  cannot succeed never runs it, and a block that raises leaves the variable
 *  untouched.
 */
static VALUE
shared_var_compare_and_swap(VALUE self, VALUE expected, VALUE value)
{
    struct shared_var *sv = shared_var_ptr(self);
    VALUE current;

    shared_var_check(sv->type, value);

    if (!rb_block_given_p()) {
        return SV_CAS_ACQ_REL(sv->value, expected, value) == expected ? Qtrue : Qfalse;
    }

    /* The block runs between the load and the store, so it can allocate, raise,
     * collect garbage and even touch this same variable.  None of that can
     * corrupt anything: the store below is a compare-and-swap against `current`,
     * which fails if the slot moved on, and `current` is live on the machine
     * stack for as long as it is needed. */
    current = SV_LOAD_ACQUIRE(sv->value);
    if (!RTEST(rb_yield_values(2, current, expected))) {
        return Qfalse;
    }
    return SV_CAS_ACQ_REL(sv->value, current, value) == current ? Qtrue : Qfalse;
}

/*
 *  call-seq:
 *     shared_var.type -> Module
 *
 *  The type every value in this variable is.
 */
static VALUE
shared_var_type(VALUE self)
{
    return shared_var_ptr(self)->type;
}

static VALUE
shared_var_inspect(VALUE self)
{
    struct shared_var *sv;

    TypedData_Get_Struct(self, struct shared_var, &shared_var_data_type, sv);
    if (NIL_P(sv->type)) {
        return rb_sprintf("#<%"PRIsVALUE" (uninitialized)>", rb_obj_class(self));
    }
    return rb_sprintf("#<%"PRIsVALUE" %"PRIsVALUE" %+"PRIsVALUE">",
                      rb_obj_class(self), sv->type,
                      SV_LOAD_ACQUIRE(sv->value));
}

void
Init_shared_var(void)
{
    rb_ext_ractor_safe(true);   /* every method here is safe from any Ractor */

    rb_cRactorSharedVar = rb_define_class_under(rb_cRactor, "SharedVar", rb_cObject);
    rb_define_alloc_func(rb_cRactorSharedVar, shared_var_alloc);
    rb_define_method(rb_cRactorSharedVar, "initialize", shared_var_initialize, 2);
    rb_define_method(rb_cRactorSharedVar, "get", shared_var_get, 0);
    rb_define_method(rb_cRactorSharedVar, "set", shared_var_set, 1);
    rb_define_method(rb_cRactorSharedVar, "swap", shared_var_swap, 1);
    rb_define_method(rb_cRactorSharedVar, "compare_and_swap", shared_var_compare_and_swap, 2);
    rb_define_method(rb_cRactorSharedVar, "type", shared_var_type, 0);
    rb_define_method(rb_cRactorSharedVar, "inspect", shared_var_inspect, 0);
}
