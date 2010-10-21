
#ifndef __SERIALIZER_LOG_DATA_BLOCK_MANAGER_HPP__
#define __SERIALIZER_LOG_DATA_BLOCK_MANAGER_HPP__

#include "arch/arch.hpp"
#include "config/cmd_args.hpp"
#include "containers/priority_queue.hpp"
#include "containers/two_level_array.hpp"
#include "containers/bitset.hpp"
#include "extents/extent_manager.hpp"
#include "log_serializer_callbacks.hpp"
#include "serializer/types.hpp"
#include <bitset>
#include <functional>
#include <queue>
#include <utility>
#include <sys/time.h>

class data_block_manager_t {

public:
    data_block_manager_t(log_serializer_t *ser, log_serializer_dynamic_config_t *dynamic_config, extent_manager_t *em, size_t _block_size)
        : shutdown_callback(NULL), state(state_unstarted), serializer(ser),
          dynamic_config(dynamic_config), extent_manager(em), block_size(_block_size),
          gc_state(extent_manager->extent_size),
          pm_garbage_ratio("garbage_ratio", &gc_stats, perfmon_combiner_sum,
                           perfmon_weighted_average_transformer)
    {
        assert(dynamic_config);
        assert(serializer);
        assert(extent_manager);
    }
    ~data_block_manager_t() {
        assert(state == state_unstarted || state == state_shut_down);
    }

public:
    struct metablock_mixin_t {
        off64_t last_data_extent;
        unsigned int blocks_in_last_data_extent;
    };

    /* When initializing the database from scratch, call start() with just the database FD. When
    restarting an existing database, call start() with the last metablock. */

public:
    /* data to be serialized to disk with each block */
    struct buf_data_t {
        ser_block_id_t block_id;
    };

public:
    void start_new(direct_file_t *dbfile);
    void start_existing(direct_file_t *dbfile, metablock_mixin_t *last_metablock);

public:
    bool read(off64_t off_in, void *buf_out, iocallback_t *cb);

    /* The offset that the data block manager chose will be left in off_out as soon as write()
    returns. The callback will be called when the data is actually on disk and it is safe to reuse
    the buffer. */
    bool write(void *buf_in, ser_block_id_t block_id, off64_t *off_out, iocallback_t *cb);

public:
    /* exposed gc api */
    /* mark a buffer as garbage */
    void mark_garbage(off64_t);

    bool is_extent_in_use(unsigned int extent_id) {
        return entries.get(extent_id) != NULL;
    }

    /* r{start,stop}_reconstruct functions for safety */
    void start_reconstruct();
    void mark_live(off64_t);
    void end_reconstruct();

    /* garbage collect the extents which meet the gc_criterion */
    void start_gc();

    /* take step in gcing */
    void run_gc();

public:
    void prepare_metablock(metablock_mixin_t *metablock);
    bool do_we_want_to_start_gcing() const;

public:
    struct shutdown_callback_t {
        virtual void on_datablock_manager_shutdown() = 0;
    };
    // The shutdown_callback_t may destroy the data_block_manager.
    bool shutdown(shutdown_callback_t *cb);

public:

    struct gc_disable_callback_t {
        virtual void on_gc_disabled() = 0;
    };

    // Always calls the callback, returns true if the callback has
    // already been called.
    bool disable_gc(gc_disable_callback_t *cb);

    // Enables gc, immediately.
    void enable_gc();

private:
    void actually_shutdown();
    // This is permitted to destroy the data_block_manager.
    shutdown_callback_t *shutdown_callback;

private:
    enum state_t {
        state_unstarted,
        state_ready,
        state_shutting_down,
        state_shut_down
    } state;

    log_serializer_t *serializer;

    log_serializer_dynamic_config_t *dynamic_config;

    extent_manager_t *extent_manager;

    direct_file_t *dbfile;
    size_t block_size;

    off64_t gimme_a_new_offset();

private:
    
    struct gc_entry;
    
    struct Less {
        bool operator() (const gc_entry *x, const gc_entry *y);
    };

    // Identifies an extent, the time we started writing to the
    // extent, whether it's the extent we're currently writing to, and
    // describes blocks are garbage.
    struct gc_entry :
        public alloc_mixin_t<tls_small_obj_alloc_accessor<alloc_t>, gc_entry>,
        public intrusive_list_node_t<gc_entry>
    {
    public:
        typedef uint64_t timestamp_t;
    
        off64_t offset; /* !< the offset that this extent starts at */
        bitset_t g_array; /* !< bit array for whether or not each block is garbage */
        timestamp_t timestamp; /* !< when we started writing to the extent */
        priority_queue_t<gc_entry*, Less>::entry_t *our_pq_entry; /* !< The PQ entry pointing to us */
        
        enum state_t {
            // It has been, or is being, reconstructed from data on disk.
            state_reconstructing,
            // We are currently putting things on this extent. It is equal to last_data_extent.
            state_active,
            // Not active, but not a GC candidate yet. It is in young_extent_queue.
            state_young,
            // Candidate to be GCed. It is in gc_pq.
            state_old,
            // Currently being GCed. It is equal to gc_state.current_entry.
            state_in_gc,
        } state;
        
    public:
        
        /* This constructor is for starting a new extent. */
        explicit gc_entry(extent_manager_t *em, size_t blocks_per_extent)
            : g_array(blocks_per_extent)
        {
            offset = em->gen_extent();
            g_array.set();
            timestamp = current_timestamp();
            state = state_active;
            our_pq_entry = NULL;
        }
        
        /* This constructor is for reconstructing extents that the LBA tells us contained
        data blocks. */
        explicit gc_entry(off64_t off, size_t blocks_per_extent)
            : g_array(blocks_per_extent)
        {
            offset = off;
            g_array.set();
            timestamp = -1;
            state = state_reconstructing;
            our_pq_entry = NULL;
        }
        
        void print() {
#ifndef NDEBUG
            debugf("gc_entry:\n");
            debugf("offset: %ld\n", offset);
            for (unsigned int i = 0; i < g_array.size(); i++)
                debugf("%.8x:\t%d\n", (unsigned int) (offset + (i * DEVICE_BLOCK_SIZE)), g_array.test(i));
            debugf("\n");
            debugf("\n");
#endif
        }

        // Returns the current timestamp in microseconds.
        static timestamp_t current_timestamp() {
            struct timeval t;
            assert(0 == gettimeofday(&t, NULL));
            return uint64_t(t.tv_sec) * (1000 * 1000) + t.tv_usec;
        }
    };
    
    /* Contains a pointer to every gc_entry, regardless of what its current state is */
    two_level_array_t<gc_entry*, MAX_DATA_EXTENTS> entries;
    
    /* Contains every extent in the gc_entry::state_reconstructing state */
    intrusive_list_t< gc_entry > reconstructed_extents;
    
    /* Contains the extent in the gc_entry::state_active state */
    gc_entry *last_data_extent;
    unsigned blocks_in_last_data_extent;
    
    /* Contains every extent in the gc_entry::state_young state */
    intrusive_list_t< gc_entry > young_extent_queue;
    
    /* Contains every extent in the gc_entry::state_old state */
    priority_queue_t<gc_entry*, Less> gc_pq;

    // Tells if we should keep gc'ing, being told the next extent that
    // would be gc'ed.
    bool should_we_keep_gcing(const gc_entry&) const;

    // Pops things off young_extent_queue that are no longer young.
    void mark_unyoung_entries();
    
    // Pops the last gc_entry off young_extent_queue and declares it
    // to be not young.
    void remove_last_unyoung_entry();

private:
    /* internal garbage collection structures */
    struct gc_read_callback_t : public iocallback_t {
        data_block_manager_t *parent;
        void on_io_complete(event_t *) {
            parent->run_gc();
        }
    };

    struct gc_write_callback_t : public _write_txn_callback_t{
        public:
            data_block_manager_t *parent;
            void on_serializer_write_txn() {
                parent->run_gc();
            }
    };

    enum gc_step {
        gc_reconstruct, /* reconstructing on startup */
        gc_ready, /* ready to start */
        gc_read,  /* waiting for reads, sending out writes */
        gc_write, /* waiting for writes */
    };

    struct gc_state_t {
    private:
        gc_step step_;               /* !< which step we're on.  See set_step.  */
    public:
        bool should_be_stopped;      /* !< whether gc is/should be
                                       stopped, and how many people
                                       think so */
        int refcount;               /* !< outstanding io reqs */
        char *gc_blocks;            /* !< buffer for blocks we're transferring */
        gc_entry *current_entry;    /* !< entry we're currently GCing */
        data_block_manager_t::gc_read_callback_t gc_read_callback;
        data_block_manager_t::gc_write_callback_t gc_write_callback;
        data_block_manager_t::gc_disable_callback_t *gc_disable_callback;

        gc_state_t(size_t extent_size) : step_(gc_ready), should_be_stopped(0), refcount(0), current_entry(NULL)
        {
            memalign_alloc_t<DEVICE_BLOCK_SIZE> blocks_buffer_allocator;
            /* TODO this is excessive as soon as we have a bound on how much space we need we should allocate less */
            gc_blocks = (char *) blocks_buffer_allocator.malloc(extent_size);
        }
        ~gc_state_t() {
            free(gc_blocks);
        }
        inline gc_step step() const { return step_; }

        // Sets step_, and calls gc_disable_callback if relevant.
        void set_step(gc_step next_step) {
            if (should_be_stopped && next_step == gc_ready && (step_ == gc_read || step_ == gc_write)) {
                assert(gc_disable_callback);
                gc_disable_callback->on_gc_disabled();
                gc_disable_callback = NULL;
            }

            step_ = next_step;
        }
    } gc_state;

private:
    /* \brief structure to keep track of global stats about the data blocks
     */
    struct gc_stats_t {
        int old_total_blocks;
        int old_garbage_blocks;
        gc_stats_t()
            : old_total_blocks(0), old_garbage_blocks(0)
        {}
    } gc_stats;

    friend std::ostream& operator<<(std::ostream& out, const data_block_manager_t::gc_stats_t& stats);

    perfmon_var_t<gc_stats_t> pm_garbage_ratio;

public:
    /* \brief ratio of garbage to blocks in the system
     */
    float garbage_ratio() const;

    int64_t garbage_ratio_total_blocks() const { return gc_stats.old_garbage_blocks; }
    int64_t garbage_ratio_garbage_blocks() const { return gc_stats.old_garbage_blocks; }

private:
    DISABLE_COPYING(data_block_manager_t);
};

std::ostream& operator<<(std::ostream&, const data_block_manager_t::gc_stats_t&);

#endif /* __SERIALIZER_LOG_DATA_BLOCK_MANAGER_HPP__ */
