#include "ruby/ruby.h"
#include "ruby/atomic.h"
#include "ruby/ractor.h"

/* Ractor::LockFree::Hash - a shareable key/value table that every Ractor reads
 * and writes without a lock.
 *
 * The table is open-addressed with triangular probing, in the shape of Ruby's
 * own concurrent_set.c, and every slot is three words:
 *
 *     hash    0 while the slot has never been used, otherwise the key's hash
 *     key     Qundef until a writer claims the slot, then that key forever
 *     value   Qundef until the claimer publishes, then the current value
 *
 * Each of those three words only ever moves forward, and each move is one
 * compare-and-swap, which is what lets readers and writers walk the same slots
 * at the same time with nothing to take and nothing to wait for:
 *
 *     hash:   0 -> h                         (once)
 *     key:    Qundef -> k                    (once)
 *     value:  Qundef -> v -> v' ... -> MOVED (MOVED is final)
 *
 * A writer claims a slot in that order -- hash, then key, then value -- so a
 * reader that can see a key can trust the hash beside it, and a reader that can
 * see a value can trust the key beside it.  A slot whose key is still Qundef
 * belongs to an insert that has not named its key yet, and one whose value is
 * still Qundef belongs to an insert that has not finished: both read as "not
 * here", which is the truth, because that insert has not returned to its caller
 * either.
 *
 * Two writers that want the same free slot both try the same compare-and-swap
 * on `key`, and the loser goes back to the same slot to look at what the winner
 * put there.  That is what keeps one key in one slot: probing is a pure
 * function of the hash and the capacity, so any two writers for a key walk the
 * same slots in the same order and meet on the first one either of them can
 * claim.
 *
 * ## Growing
 *
 * A table never grows in place.  When one passes a 3/4 load it gets a successor
 * twice its size, linked from `next`, and the pairs are carried over one slot at
 * a time by whichever Ractors happen to be writing -- there is no rebuild step
 * that anybody waits for, and no Ractor is ever stopped.
 *
 * MOVED, the last state of a `value`, is what makes that safe.  It means "this
 * slot is finished; the successor is where this key lives now", and because a
 * writer publishes with a compare-and-swap against the value it read, a write
 * to a finished slot cannot succeed:
 *
 *   - a slot is carried over *before* it is marked MOVED, so a reader sent to
 *     the successor by a MOVED always finds the key there;
 *   - a write that lands before the mark is in the old table, is visible there,
 *     and is carried over by the same thread that marks the slot, which retries
 *     until its mark and its copy agree;
 *   - a write that lands after the mark fails, and is retried against the
 *     successor.
 *
 * So every key has one authoritative slot at every instant, the handover
 * between two slots for the same key is a single compare-and-swap, and no write
 * is ever lost or applied out of order.
 *
 * Carrying one slot over is claimed with a fetch-and-add on `claim`, so exactly
 * one Ractor is responsible for each slot, and that work is plain C -- no
 * allocation, no Ruby call, no interrupt checkpoint -- so a claim is always
 * finished by the Ractor that takes it.  When the last slot is marked, whoever
 * marked it swaps the successor into place with one compare-and-swap, and the
 * old table becomes garbage; a Ractor still reading it keeps it alive through
 * the machine stack, the way rb_concurrent_set does.
 *
 * ## What "lock-free" buys and costs here
 *
 * #get and #put are lock-free, not wait-free: an operation retries when it
 * loses a race, so the table as a whole always makes progress but one caller
 * can be made to go round again.  Nothing blocks, though -- there is no lock to
 * hold, so a Ractor killed or interrupted mid-operation leaves the table in a
 * state the next caller can use, and there is no deadlock to have.
 *
 * The one place this file allocates is a table, once per doubling, in whichever
 * Ractor first sees the load factor; making that table shareable takes a
 * VM-internal lock briefly.  No #get or #put waits on it.
 *
 * Both operations run the key's #hash and #eql?, which is Ruby code, so both
 * can allocate, raise, collect garbage and even re-enter this table.  None of
 * that can hurt: nothing is held, and a raise leaves at worst a claimed slot
 * that nobody will look up, since a key is only claimed once the comparisons
 * are done.
 *
 * Ordering is release/acquire, for the reason documented at length in
 * shared_var.c: a writer's stores are finished before another Ractor can see
 * the pointer to them, per-slot ordering comes from each atomic's own
 * modification order rather than from SEQ_CST, and nothing here promises a
 * global order across different slots.
 */

static VALUE rb_cRactorLockFreeHash;

/* The class of a table.  Anonymous and unreachable from Ruby: tables are
 * internal, and this only exists because rb_ractor_make_shareable freezes
 * through #freeze, which needs a class to dispatch on. */
static VALUE lfh_cTable;

/* ---------------------------------------------------------------------------
 * Atomics
 *
 * Same arrangement as shared_var.c: the memory order is a parameter on the
 * rbimpl_atomic_* inline functions that ruby/atomic.h ships, and where those
 * are absent these fall back to the public SEQ_CST macros, which are stronger
 * than needed and so still correct.
 * ------------------------------------------------------------------------- */
#if defined(RBIMPL_ATOMIC_ACQUIRE) && defined(RBIMPL_ATOMIC_RELEASE) && \
    defined(RBIMPL_ATOMIC_ACQ_REL) && defined(RBIMPL_ATOMIC_RELAXED)
# define LFH_LOAD(var)              rbimpl_atomic_value_load(&(var), RBIMPL_ATOMIC_ACQUIRE)
# define LFH_STORE(var, val)        rbimpl_atomic_value_store(&(var), (val), RBIMPL_ATOMIC_RELEASE)
# define LFH_CAS(var, old, new) \
    rbimpl_atomic_value_cas(&(var), (old), (new), RBIMPL_ATOMIC_ACQ_REL, RBIMPL_ATOMIC_ACQUIRE)
/* A counter read for its own sake, with no data hanging off it. */
# define LFH_CNT_LOAD(var)          rbimpl_atomic_load(&(var), RBIMPL_ATOMIC_RELAXED)
# define LFH_CNT_ADD(var, n)        rbimpl_atomic_fetch_add(&(var), (n), RBIMPL_ATOMIC_RELAXED)
/* `sealed` is the one counter whose value means something about other Ractors'
 * writes -- reaching the capacity is what says every pair has been carried over
 * -- so it is published and read like a pointer would be. */
# define LFH_CNT_LOAD_ACQ(var)      rbimpl_atomic_load(&(var), RBIMPL_ATOMIC_ACQUIRE)
# define LFH_CNT_ADD_REL(var, n)    rbimpl_atomic_fetch_add(&(var), (n), RBIMPL_ATOMIC_RELEASE)
#else
# define LFH_LOAD(var)              ((VALUE)RUBY_ATOMIC_PTR_LOAD(var))
# define LFH_STORE(var, val)        RUBY_ATOMIC_VALUE_SET((var), (val))
# define LFH_CAS(var, old, new)     RUBY_ATOMIC_VALUE_CAS((var), (old), (new))
# define LFH_CNT_LOAD(var)          RUBY_ATOMIC_LOAD(var)
# define LFH_CNT_ADD(var, n)        RUBY_ATOMIC_FETCH_ADD((var), (n))
# define LFH_CNT_LOAD_ACQ(var)      RUBY_ATOMIC_LOAD(var)
# define LFH_CNT_ADD_REL(var, n)    RUBY_ATOMIC_FETCH_ADD((var), (n))
#endif

/* ---------------------------------------------------------------------------
 * Slot states
 * ------------------------------------------------------------------------- */

/* A `hash` of 0 means the slot has never been used.  No key is allowed to hash
 * to it, and `next` uses the same spelling for "there is no successor". */
#define LFH_FREE  ((VALUE)0)

/* Qundef is the one VALUE that cannot reach Ruby, so it can stand for "no key
 * yet" and "no value yet" without colliding with a key or value a caller is
 * entitled to store -- including false, whose VALUE is 0. */
#define LFH_UNSET RUBY_Qundef

/* "Carried over to the successor; this slot is finished."  Needs to be
 * distinguishable from every value a caller may store, so it is a private
 * object that no Ruby code can name, let alone hand to #put. */
static VALUE lfh_moved;
#define LFH_MOVED (lfh_moved)

#define LFH_DEFAULT_CAPACITY 16

/* Slots carried over per operation while a resize is in flight.  Writes are
 * what fill a table, so paying a bounded amount of the copy per write is what
 * keeps the copy ahead of the filling without any single caller paying for all
 * of it: a table needs capacity/64 writes to be emptied, and has capacity/4
 * writes' worth of room left when the copy starts.
 *
 * A read pays too, but only a read that had to go on to the successor -- one
 * that is already walking two tables instead of one.  Those are the reads a
 * finished resize would make faster, and they are what finishes it when the
 * writes stop: a table that fills up and then only gets read would otherwise
 * sit half-copied, with every lookup paying for both halves forever.  A read
 * that finds what it wants in the table it started on does no copying at all. */
#define LFH_CARRY_PER_WRITE 64
#define LFH_CARRY_PER_READ  8

/* Bigger than the biggest table anyone can afford; keeps the doubling below
 * from wrapping. */
#define LFH_MAX_CAPACITY (1U << 30)

struct lfh_entry {
    VALUE hash;
    VALUE key;
    VALUE value;
};

struct lfh_table {
    unsigned int capacity;      /* a power of two, fixed for the table's life */
    rb_atomic_t size;           /* claimed keys; a hint, and may run ahead */
    rb_atomic_t sealed;         /* slots whose value has reached MOVED; exact */
    rb_atomic_t claim;          /* next slot index to hand to a helper */
    VALUE next;                 /* the successor, LFH_FREE until a resize starts */
    struct lfh_entry *entries;
};

struct lfh_map {
    VALUE table;                /* the table #get and #put start from */
};

/* ---------------------------------------------------------------------------
 * Tables
 * ------------------------------------------------------------------------- */

static void
lfh_table_mark(void *ptr)
{
    struct lfh_table *t = ptr;
    VALUE next = LFH_LOAD(t->next);
    unsigned int i;

    if (next != LFH_FREE) rb_gc_mark(next);

    /* rb_gc_mark, not rb_gc_mark_movable: another Ractor may be holding a key
     * or a value it loaded out of a slot, and compaction rewriting the slot
     * under it would leave it looking at a forwarding address. */
    for (i = 0; i < t->capacity; i++) {
        VALUE key = LFH_LOAD(t->entries[i].key);
        VALUE value = LFH_LOAD(t->entries[i].value);

        if (key != LFH_UNSET) rb_gc_mark(key);
        if (value != LFH_UNSET && value != LFH_MOVED) rb_gc_mark(value);
    }
}

static void
lfh_table_free(void *ptr)
{
    struct lfh_table *t = ptr;

    ruby_xfree(t->entries);
}

static size_t
lfh_table_memsize(const void *ptr)
{
    const struct lfh_table *t = ptr;

    return sizeof(struct lfh_table) + (size_t)t->capacity * sizeof(struct lfh_entry);
}

/* Deliberately not RUBY_TYPED_WB_PROTECTED.  Unprotected, the table counts as
 * always remembered, so every marking walks the slots again and finds a key or
 * value stored after the walk began.  Protected is not impossible -- it would
 * mean rb_gc_writebarrier() on the table after every successful slot CAS -- but
 * it puts a barrier call on the write path to save re-marking, and a slot whose
 * store missed its barrier loses an object the table still holds.
 *
 * Where a local GC pins shareable objects this is moot with more than one
 * Ractor: keys and values are shareable, so they are marked whatever this type
 * says, and only a global GC can free them.  It is not moot with one Ractor,
 * where that pinning is skipped, nor on rubies without it. */
static const rb_data_type_t lfh_table_type = {
    "Ractor::LockFree::Hash::table",
    {lfh_table_mark, lfh_table_free, lfh_table_memsize, NULL},
    0, 0, RUBY_TYPED_FREE_IMMEDIATELY | RUBY_TYPED_FROZEN_SHAREABLE
};

static struct lfh_table *
lfh_table_ptr(VALUE table_obj)
{
    return RTYPEDDATA_GET_DATA(table_obj);
}

static VALUE
lfh_table_new(unsigned int capacity)
{
    struct lfh_table *t;
    VALUE obj = TypedData_Make_Struct(lfh_cTable, struct lfh_table, &lfh_table_type, t);
    unsigned int i;

    /* capacity stays 0 until the entries exist, so a raise from the allocation
     * leaves an object whose mark and memsize hooks read nothing. */
    t->entries = ALLOC_N(struct lfh_entry, capacity);
    t->capacity = capacity;

    for (i = 0; i < capacity; i++) {
        t->entries[i].hash = LFH_FREE;
        t->entries[i].key = LFH_UNSET;
        t->entries[i].value = LFH_UNSET;
    }

    /* Every Ractor is going to reach this table, so it has to be shareable
     * before the pointer to it is published.  rb_ractor_make_shareable is the
     * only way to say so from an extension; on an empty table it freezes the
     * object and finds nothing to walk. */
    rb_ractor_make_shareable(obj);
    return obj;
}

/* ---------------------------------------------------------------------------
 * Probing
 *
 * Triangular probing: idx, idx+1, idx+3, idx+6, ...  With a power-of-two
 * capacity that visits every slot exactly once, so a walk of `capacity` steps
 * has seen the whole table and can stop.
 * ------------------------------------------------------------------------- */

struct lfh_probe {
    unsigned int idx;
    unsigned int d;
    unsigned int mask;
};

static unsigned int
lfh_probe_start(struct lfh_probe *probe, const struct lfh_table *t, VALUE hash)
{
    probe->d = 0;
    probe->mask = t->capacity - 1;
    probe->idx = (unsigned int)(hash & (VALUE)probe->mask);
    return probe->idx;
}

static unsigned int
lfh_probe_next(struct lfh_probe *probe)
{
    probe->d++;
    probe->idx = (probe->idx + probe->d) & probe->mask;
    return probe->idx;
}

/* ---------------------------------------------------------------------------
 * Keys
 * ------------------------------------------------------------------------- */

/* Runs the key's #hash, so it can do anything Ruby can do.  Called once per
 * operation, before any slot is touched, and never again -- a key's hash cannot
 * change under us because a key that reaches a slot is shareable and therefore
 * frozen. */
static VALUE
lfh_hash_for(VALUE key)
{
    VALUE hash = (VALUE)NUM2LONG(rb_hash(key));

    return hash == LFH_FREE ? (VALUE)1 : hash;
}

/* #eql? and #hash, the pair Ruby's own Hash matches keys with, so two equal
 * frozen strings are one key.  Identity first, because that is the common case
 * and it needs no Ruby call. */
static bool
lfh_key_eq(VALUE a, VALUE b)
{
    return a == b || rb_eql(a, b);
}

static void
lfh_check_shareable(VALUE obj, const char *what)
{
    if (RB_UNLIKELY(!rb_ractor_shareable_p(obj))) {
        rb_raise(rb_eArgError, "only shareable objects are allowed as a %s, got an unshareable %"PRIsVALUE,
                 what, rb_obj_class(obj));
    }
}

/* ---------------------------------------------------------------------------
 * Growing
 * ------------------------------------------------------------------------- */

static bool
lfh_load_factor_reached(struct lfh_table *t)
{
    /* 3/4 full.  The size may run ahead of the truth while inserts are in
     * flight, which only ever makes this early. */
    return (uint64_t)LFH_CNT_LOAD(t->size) * 4 >= (uint64_t)t->capacity * 3;
}

/* The successor, made if there is not one yet.  Allocates, so it can collect
 * garbage and raise; both are fine, because it is only ever called with nothing
 * claimed and nothing half-written. */
static VALUE
lfh_ensure_next(VALUE table_obj, struct lfh_table *t)
{
    VALUE next = LFH_LOAD(t->next), fresh, prev;

    if (next != LFH_FREE) return next;

    if (RB_UNLIKELY(t->capacity >= LFH_MAX_CAPACITY)) {
        rb_raise(rb_eRuntimeError, "%"PRIsVALUE" cannot grow past %u slots",
                 rb_cRactorLockFreeHash, LFH_MAX_CAPACITY);
    }
    fresh = lfh_table_new(t->capacity * 2);

    prev = LFH_CAS(t->next, LFH_FREE, fresh);
    RB_GC_GUARD(table_obj);
    /* A lost race leaves `fresh` to the garbage collector and uses the winner's
     * table, so every Ractor agrees on which successor this is. */
    return prev == LFH_FREE ? fresh : prev;
}

/* The successor's slot for `key`, claimed if it is not claimed yet.
 *
 * Matches by identity, which is enough and is what keeps this function free of
 * Ruby calls: the only keys in the successor are ones carried over from this
 * table, which are unique already, and keys put there by writers a MOVED sent
 * over -- and a key is only MOVED after it has been carried, so no writer can
 * have put an equal key there ahead of us.
 *
 * Plain C throughout: no allocation, no Ruby, no interrupt checkpoint.  That is
 * what makes a claimed slot one the claiming Ractor always finishes.
 */
static struct lfh_entry *
lfh_reserve(struct lfh_table *t, VALUE hash, VALUE key)
{
    struct lfh_probe probe;
    unsigned int idx = lfh_probe_start(&probe, t, hash);
    unsigned int visited = 0;

    while (visited <= t->capacity) {
        struct lfh_entry *e = &t->entries[idx];
        VALUE entry_hash = LFH_LOAD(e->hash);

        if (entry_hash == LFH_FREE) {
            if (LFH_CAS(e->hash, LFH_FREE, hash) != LFH_FREE) continue;
            entry_hash = hash;
        }
        if (entry_hash == hash) {
            VALUE entry_key = LFH_LOAD(e->key);

            if (entry_key == LFH_UNSET) {
                if (LFH_CAS(e->key, LFH_UNSET, key) != LFH_UNSET) continue;
                LFH_CNT_ADD(t->size, 1);
                return e;
            }
            if (entry_key == key) return e;
        }
        idx = lfh_probe_next(&probe);
        visited++;
    }

    /* A successor is twice the size of the table being emptied into it and only
     * receives that table's pairs, so it cannot fill up. */
    rb_bug("Ractor::LockFree::Hash: no room in the successor table");
}

/* Carry one slot over and mark it MOVED.  Exactly one Ractor runs this for a
 * given slot, which is what the fetch-and-add on `claim` buys. */
static void
lfh_carry_slot(struct lfh_table *t, unsigned int i, struct lfh_table *next)
{
    struct lfh_entry *e = &t->entries[i];
    struct lfh_entry *ne;
    VALUE hash, key, value, carried;

    for (;;) {
        value = LFH_LOAD(e->value);

        if (value == LFH_MOVED) return;             /* nothing left to do */
        if (value != LFH_UNSET) break;              /* a pair to carry over */

        /* Nothing has ever been published here: an unused slot, or an insert
         * still in flight.  Marking it turns that insert away to the successor,
         * which is the only table anybody will read once this one is retired. */
        if (LFH_CAS(e->value, LFH_UNSET, LFH_MOVED) == LFH_UNSET) {
            LFH_CNT_ADD_REL(t->sealed, 1);
            return;
        }
        /* The insert published first.  Carry its value instead. */
    }

    /* A published value means the key was claimed before it, and a claimed key
     * never changes, so this reads the key this slot will always have. */
    hash = LFH_LOAD(e->hash);
    key = LFH_LOAD(e->key);
    ne = lfh_reserve(next, hash, key);

    carried = LFH_UNSET;
    for (;;) {
        VALUE prev;

        if (value != carried) {
            /* Until the mark below lands, this thread is the only writer of the
             * successor's slot for this key, so this cannot lose. */
            LFH_CAS(ne->value, carried, value);
            carried = value;
        }

        prev = LFH_CAS(e->value, value, LFH_MOVED);
        if (prev == value) {
            LFH_CNT_ADD_REL(t->sealed, 1);
            return;
        }
        /* A writer beat the mark, in this table, where it is visible.  Its value
         * is the newer one, so carry that and try the mark again. */
        value = prev;
    }
}

/* Carry over at most `budget` slots, and retire the table if that finished the
 * job. */
static void
lfh_help_carry(struct lfh_map *map, VALUE table_obj, struct lfh_table *t,
               VALUE next_obj, unsigned int budget)
{
    struct lfh_table *next = lfh_table_ptr(next_obj);
    unsigned int n;

    for (n = 0; n < budget; n++) {
        rb_atomic_t i;

        if ((unsigned int)LFH_CNT_LOAD(t->claim) >= t->capacity) break;
        i = LFH_CNT_ADD(t->claim, 1);
        if ((unsigned int)i >= t->capacity) break;

        lfh_carry_slot(t, (unsigned int)i, next);
    }

    if ((unsigned int)LFH_CNT_LOAD_ACQ(t->sealed) >= t->capacity) {
        /* Every pair is in the successor and every slot here refuses writes, so
         * this table has nothing left to say.  A failed swap means somebody else
         * said it first. */
        LFH_CAS(map->table, table_obj, next_obj);
    }
    RB_GC_GUARD(next_obj);
}

/* Called after a write lands, to keep a resize moving and to start one when the
 * write was the one that filled the table up. */
static void
lfh_after_write(struct lfh_map *map, VALUE table_obj, struct lfh_table *t, bool claimed_key)
{
    VALUE next = LFH_LOAD(t->next);

    if (next == LFH_FREE) {
        if (!claimed_key || !lfh_load_factor_reached(t)) return;
        next = lfh_ensure_next(table_obj, t);
    }
    lfh_help_carry(map, table_obj, t, next, LFH_CARRY_PER_WRITE);
}

/* ---------------------------------------------------------------------------
 * The map
 * ------------------------------------------------------------------------- */

static void
lfh_map_mark(void *ptr)
{
    struct lfh_map *m = ptr;
    VALUE table = LFH_LOAD(m->table);

    /* Only the current table.  A retired one is kept alive by the machine stack
     * of whichever Ractor is still reading it, and is collected as soon as none
     * is -- which is the whole of this table's memory reclamation. */
    if (table != LFH_FREE) rb_gc_mark(table);
}

static size_t
lfh_map_memsize(const void *ptr)
{
    return sizeof(struct lfh_map);
}

/* Not RUBY_TYPED_WB_PROTECTED for the same reason as the table: `table` is
 * swapped with a compare-and-swap by whichever Ractor finishes a resize, and
 * being re-marked every cycle is what makes the successor safe to install
 * without a barrier on that path. */
static const rb_data_type_t lfh_map_type = {
    "Ractor::LockFree::Hash",
    {lfh_map_mark, RUBY_TYPED_DEFAULT_FREE, lfh_map_memsize, NULL},
    0, 0, RUBY_TYPED_FREE_IMMEDIATELY | RUBY_TYPED_FROZEN_SHAREABLE
};

static struct lfh_map *
lfh_map_ptr(VALUE self)
{
    struct lfh_map *m;

    TypedData_Get_Struct(self, struct lfh_map, &lfh_map_type, m);
    /* .allocate without .new leaves no table. */
    if (RB_UNLIKELY(LFH_LOAD(m->table) == LFH_FREE)) {
        rb_raise(rb_eTypeError, "uninitialized %"PRIsVALUE, rb_obj_class(self));
    }
    return m;
}

static VALUE
lfh_alloc(VALUE klass)
{
    struct lfh_map *m;

    return TypedData_Make_Struct(klass, struct lfh_map, &lfh_map_type, m);
}

/*
 *  call-seq:
 *     Ractor::LockFree::Hash.new -> hash
 *
 *  Makes an empty table, frozen and shareable, that any Ractor may read and
 *  write.
 */
static VALUE
lfh_initialize(VALUE self)
{
    struct lfh_map *m;

    /* Without this, send(:initialize) on a table already in use would empty it
     * under whoever was reading it. */
    rb_check_frozen(self);
    TypedData_Get_Struct(self, struct lfh_map, &lfh_map_type, m);

    LFH_STORE(m->table, lfh_table_new(LFH_DEFAULT_CAPACITY));

    /* Nothing in the object itself changes after this, so it can be frozen and
     * handed to any Ractor; RUBY_TYPED_FROZEN_SHAREABLE is what allows that for
     * a T_DATA.  The slots inside are not part of its Ruby-visible state. */
    rb_obj_freeze(self);
    rb_ractor_make_shareable(self);
    return self;
}

/* The value for `key`, or `ifnone` when the table has no such key. */
static VALUE
lfh_lookup(VALUE self, VALUE key, VALUE ifnone)
{
    struct lfh_map *m = lfh_map_ptr(self);
    VALUE hash, table_obj;

    lfh_check_shareable(key, "key");
    hash = lfh_hash_for(key);           /* runs Ruby, so before any slot is read */
    table_obj = LFH_LOAD(m->table);

    for (;;) {
        struct lfh_table *t = lfh_table_ptr(table_obj);
        struct lfh_probe probe;
        unsigned int idx = lfh_probe_start(&probe, t, hash);
        unsigned int visited;
        VALUE next;

        for (visited = 0; visited < t->capacity;
             visited++, idx = lfh_probe_next(&probe)) {
            struct lfh_entry *e = &t->entries[idx];
            VALUE entry_hash = LFH_LOAD(e->hash);
            VALUE entry_key, value;

            /* Never used, and slots are only ever used up: no insert of this key
             * could have walked past it, so the key is not further along. */
            if (entry_hash == LFH_FREE) break;
            if (entry_hash != hash) continue;

            entry_key = LFH_LOAD(e->key);
            if (entry_key == LFH_UNSET) continue;      /* an insert mid-claim */
            if (!lfh_key_eq(key, entry_key)) continue;

            value = LFH_LOAD(e->value);
            /* Claimed but not published: the insert has not returned to its own
             * caller yet, so it has not happened for this one either. */
            if (value == LFH_UNSET) break;
            if (value == LFH_MOVED) break;             /* ask the successor */

            RB_GC_GUARD(table_obj);
            return value;
        }

        /* Not in this table.  If it has a successor, the key may have been
         * carried there, or put there by a writer this table turned away. */
        next = LFH_LOAD(t->next);
        if (next == LFH_FREE) {
            RB_GC_GUARD(table_obj);
            return ifnone;
        }
        /* Walking two tables is what a half-finished resize costs, so pay a
         * little of the resize off on the way past.  Carrying a slot over is
         * plain C -- it changes nothing a caller can observe. */
        lfh_help_carry(m, table_obj, t, next, LFH_CARRY_PER_READ);
        RB_GC_GUARD(table_obj);
        table_obj = next;
    }
}

/*
 *  call-seq:
 *     hash.get(key, default = nil) -> value or default
 *
 *  The value most recently written for +key+ by any Ractor, or +default+ if the
 *  table has no such key.
 *
 *  Since nil is a value one can store, +default+ is how an absent key is told
 *  from a stored nil: pass anything the table cannot hold for that key -- a
 *  private frozen object is the usual choice -- and compare against it.
 *
 *  Never blocks.  Raises ArgumentError unless +key+ and +default+ are both
 *  shareable, +default+ because a caller cannot tell whether what came back was
 *  stored or defaulted, so both have to be equally safe to pass on.
 */
static VALUE
lfh_get(int argc, VALUE *argv, VALUE self)
{
    VALUE ifnone = Qnil;

    rb_check_arity(argc, 1, 2);
    if (argc > 1) {
        ifnone = argv[1];
        lfh_check_shareable(ifnone, "default");
    }
    return lfh_lookup(self, argv[0], ifnone);
}

/*
 *  call-seq:
 *     hash.put(key, value) -> value
 *
 *  Makes +value+ what #get(key) returns, from every Ractor, and returns it.
 *  Never blocks.
 *
 *  Raises ArgumentError unless both +key+ and +value+ are shareable, and does
 *  so before touching the table, so a rejected write leaves it as it was.
 */
static VALUE
lfh_put(VALUE self, VALUE key, VALUE value)
{
    struct lfh_map *m = lfh_map_ptr(self);
    VALUE hash, table_obj;

    lfh_check_shareable(key, "key");
    lfh_check_shareable(value, "value");
    hash = lfh_hash_for(key);
    table_obj = LFH_LOAD(m->table);

    for (;;) {
        struct lfh_table *t = lfh_table_ptr(table_obj);
        struct lfh_probe probe;
        unsigned int idx = lfh_probe_start(&probe, t, hash);
        unsigned int visited = 0;
        bool claimed_key = false;
        VALUE next;

        while (visited < t->capacity) {
            struct lfh_entry *e = &t->entries[idx];
            VALUE entry_hash = LFH_LOAD(e->hash);
            VALUE entry_key, current;

            if (entry_hash == LFH_FREE) {
                /* Take the slot for our hash.  A lost race means somebody else
                 * took it, so look again at what they took it for; the word only
                 * ever moves once, so that is one extra look, not a spin. */
                if (LFH_CAS(e->hash, LFH_FREE, hash) != LFH_FREE) continue;
                entry_hash = hash;
            }
            if (entry_hash != hash) {
                idx = lfh_probe_next(&probe);
                visited++;
                continue;
            }

            entry_key = LFH_LOAD(e->key);
            if (entry_key == LFH_UNSET) {
                /* Every writer for this key meets every other one right here. */
                if (LFH_CAS(e->key, LFH_UNSET, key) != LFH_UNSET) continue;
                LFH_CNT_ADD(t->size, 1);
                claimed_key = true;
            }
            else if (!lfh_key_eq(key, entry_key)) {
                idx = lfh_probe_next(&probe);
                visited++;
                continue;
            }

            /* This slot is this key's, now and for as long as the table lives.
             * Publish against what is there, so a write to a slot that has been
             * carried over fails instead of being lost. */
            current = LFH_LOAD(e->value);
            while (current != LFH_MOVED) {
                VALUE prev = LFH_CAS(e->value, current, value);

                if (prev == current) {
                    lfh_after_write(m, table_obj, t, claimed_key);
                    RB_GC_GUARD(table_obj);
                    return value;
                }
                current = prev;
            }
            break;      /* carried over: the successor is where this key is */
        }

        /* Either this key's slot has been carried over, or the whole table has
         * been walked without finding a slot that could take it -- and a slot
         * that cannot take a key now never will, since neither a hash nor a key
         * is ever unwritten.  The successor answers both. */
        next = LFH_LOAD(t->next);
        if (next == LFH_FREE) next = lfh_ensure_next(table_obj, t);
        lfh_help_carry(m, table_obj, t, next, LFH_CARRY_PER_WRITE);
        RB_GC_GUARD(table_obj);
        table_obj = next;
    }
}

void
Init_lockfree_hash(void)
{
    VALUE mLockFree;

    rb_ext_ractor_safe(true);   /* every method here is safe from any Ractor */

    lfh_cTable = rb_class_new(rb_cObject);
    /* Tables are T_DATA; saying so up front is what keeps the first one from
     * warning that the allocator it never uses has been undefined. */
    rb_undef_alloc_func(lfh_cTable);
    rb_gc_register_mark_object(lfh_cTable);

    lfh_moved = rb_obj_freeze(rb_obj_alloc(rb_cObject));
    rb_ractor_make_shareable(lfh_moved);
    rb_gc_register_mark_object(lfh_moved);

    mLockFree = rb_define_module_under(rb_cRactor, "LockFree");
    rb_cRactorLockFreeHash = rb_define_class_under(mLockFree, "Hash", rb_cObject);

    rb_define_alloc_func(rb_cRactorLockFreeHash, lfh_alloc);
    rb_define_method(rb_cRactorLockFreeHash, "initialize", lfh_initialize, 0);
    rb_define_method(rb_cRactorLockFreeHash, "get", lfh_get, -1);
    rb_define_method(rb_cRactorLockFreeHash, "put", lfh_put, 2);
}
