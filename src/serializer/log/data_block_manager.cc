// Copyright 2010-2013 RethinkDB, all rights reserved.
#define __STDC_FORMAT_MACROS
#include "serializer/log/data_block_manager.hpp"

#include "utils.hpp"
#include <boost/bind.hpp>

#include "arch/arch.hpp"
#include "arch/runtime/coroutines.hpp"
#include "concurrency/mutex.hpp"
#include "perfmon/perfmon.hpp"
#include "serializer/log/log_serializer.hpp"
#include "stl_utils.hpp"

// Max amount of bytes which can be read ahead in one i/o transaction (if enabled)
const int64_t APPROXIMATE_READ_AHEAD_SIZE = 32 * DEFAULT_BTREE_BLOCK_SIZE;

// TODO: Right now we perform garbage collection via the do_write() interface on the
// log_serializer_t. This leads to bugs in a couple of ways:
//
// 1. We have to be sure to get the metadata (repli timestamp, delete bit) right. The
//    data block manager shouldn't have to know about that stuff.
//
// 2. We have to special-case the serializer so that it allows us to submit
//    do_write()s during shutdown. If there were an alternative interface, it could
//    ignore or refuse our GC requests when it is shutting down.

// Later, rewrite this so that we have a special interface through which to order
// garbage collection.

data_block_manager_t::data_block_manager_t(const log_serializer_dynamic_config_t *_dynamic_config, extent_manager_t *em, log_serializer_t *_serializer, const log_serializer_on_disk_static_config_t *_static_config, log_serializer_stats_t *_stats)
    : stats(_stats), shutdown_callback(NULL), state(state_unstarted), dynamic_config(_dynamic_config),
      static_config(_static_config), extent_manager(em), serializer(_serializer),
      gc_state(), gc_stats(stats)
{
    rassert(dynamic_config != NULL);
    rassert(static_config != NULL);
    rassert(extent_manager != NULL);
    rassert(serializer != NULL);
}

data_block_manager_t::~data_block_manager_t() {
    guarantee(state == state_unstarted || state == state_shut_down);
}

void data_block_manager_t::prepare_initial_metablock(metablock_mixin_t *mb) {
    mb->active_extent = NULL_OFFSET;
    mb->blocks_in_active_extent = 0;
}

void data_block_manager_t::start_reconstruct() {
    guarantee(state == state_unstarted);
    gc_state.set_step(gc_reconstruct);
}

// Marks the block at the given offset as alive, in the appropriate
// gc_entry_t in the entries table.  (This is used when we start up, when
// everything is presumed to be garbage, until we mark it as
// non-garbage.)
void data_block_manager_t::mark_live(int64_t offset, uint32_t ser_block_size) {
    int extent_id = static_config->extent_index(offset);

    if (entries.get(extent_id) == NULL) {
        guarantee(gc_state.step() == gc_reconstruct);  // This is called at startup.

        gc_entry_t *entry = new gc_entry_t(this, extent_id * extent_manager->extent_size);
        reconstructed_extents.push_back(entry);
    }

    gc_entry_t *entry = entries.get(extent_id);
    entry->mark_live_indexwise_with_offset(offset, ser_block_size);
}

void data_block_manager_t::end_reconstruct() {
    guarantee(state == state_unstarted);
    guarantee(gc_state.step() == gc_reconstruct);
    gc_state.set_step(gc_ready);
}

void data_block_manager_t::start_existing(file_t *file, metablock_mixin_t *last_metablock) {
    guarantee(state == state_unstarted);
    dbfile = file;
    gc_io_account_nice.init(new file_account_t(file, GC_IO_PRIORITY_NICE));
    gc_io_account_high.init(new file_account_t(file, GC_IO_PRIORITY_HIGH));

    /* Reconstruct the active data block extents from the metablock. */
    const int64_t offset = last_metablock->active_extent;

    if (offset != NULL_OFFSET) {
        /* It is (perhaps) possible to have an active data block extent with no
           actual data blocks in it. In this case we would not have created a
           gc_entry_t for the extent yet. */
        if (entries.get(offset / extent_manager->extent_size) == NULL) {
            gc_entry_t *e = new gc_entry_t(this, offset);
            reconstructed_extents.push_back(e);
        }

        active_extent = entries.get(offset / extent_manager->extent_size);
        guarantee(active_extent != NULL);

        /* Turn the extent from a reconstructing extent into an active extent */
        guarantee(active_extent->state == gc_entry_t::state_reconstructing);
        reconstructed_extents.remove(active_extent);

        active_extent->make_active();
    } else {
        active_extent = NULL;
    }

    /* Convert any extents that we found live blocks in, but that are not active
    extents, into old extents */
    while (gc_entry_t *entry = reconstructed_extents.head()) {
        reconstructed_extents.remove(entry);

        guarantee(entry->state == gc_entry_t::state_reconstructing);
        entry->state = gc_entry_t::state_old;

        entry->our_pq_entry = gc_pq.push(entry);

        gc_stats.old_total_block_bytes += static_config->extent_size();
        gc_stats.old_garbage_block_bytes += entry->garbage_bytes();
    }

    state = state_ready;
}

// Computes an offset and end offset for the purposes of readahead.  Returns an interval
// that contains the [block_offset, block_offset + ser_block_size_in) interval that
// is also contained within a single extent.  boundaries is the extent's gc_entry_t's
// block_boundaries() value.  Outputs an interval that is _NOT_ fit to device block
// size boundaries.  This is used by read_ahead_offset_and_size.
void unaligned_read_ahead_interval(const int64_t block_offset,
                                   const uint32_t ser_block_size,
                                   const int64_t extent_size,
                                   const int64_t read_ahead_size,
                                   const std::vector<uint32_t> &boundaries,
                                   int64_t *const offset_out,
                                   int64_t *const end_offset_out) {
    guarantee(!boundaries.empty());
    guarantee(ser_block_size > 0);
    guarantee(boundaries.back() <= extent_size);

    const int64_t extent = floor_aligned(block_offset, extent_size);

    const int64_t raw_readahead_floor = floor_aligned(block_offset, read_ahead_size);
    const int64_t raw_readahead_ceil = raw_readahead_floor + read_ahead_size;

    // In case the extent size is configured to something weird, we cannot assume
    // that the readahead values are contained within the extent.
    const int64_t readahead_floor = std::max(raw_readahead_floor, extent);
    const int64_t readahead_ceil = std::min(raw_readahead_ceil, extent + extent_size);

    auto beg_it = std::lower_bound(boundaries.begin(), boundaries.end() - 1,
                                   readahead_floor - extent);
    auto end_it = std::lower_bound(boundaries.begin(), boundaries.end() - 1,
                                   readahead_ceil - extent);

    guarantee(beg_it < end_it);

    int64_t beg_ret = *beg_it + extent;
    int64_t end_ret = *end_it + extent;

    guarantee(beg_ret <= block_offset);

    // Since readahead_ceil > block_offset, and since the next block boundary is >=
    // block_offset + ser_block_size, we know end_ret >= block_offset +
    // ser_block_size.
    guarantee(end_ret >= block_offset + ser_block_size);
    guarantee(end_ret <= extent + extent_size);

    *offset_out = beg_ret;
    *end_offset_out = end_ret;
}

// Computes a device block size aligned interval to be read for the purposes of
// readahead.  Returns an interval that contains the [block_offset, block_offset +
// ser_block_size_in) interval.  boundaries is the extent's gc_entry_t's
// block_boundaries() value.
void read_ahead_interval(const int64_t block_offset,
                         const uint32_t ser_block_size,
                         const int64_t extent_size,
                         const int64_t read_ahead_size,
                         const int64_t device_block_size,
                         const std::vector<uint32_t> &boundaries,
                         int64_t *const offset_out,
                         int64_t *const end_offset_out) {
    int64_t unaligned_offset;
    int64_t unaligned_end_offset;
    unaligned_read_ahead_interval(block_offset, ser_block_size, extent_size,
                                  read_ahead_size, boundaries,
                                  &unaligned_offset, &unaligned_end_offset);

    *offset_out = floor_aligned(unaligned_offset, device_block_size);
    *end_offset_out = ceil_aligned(unaligned_end_offset, device_block_size);
}


void read_ahead_offset_and_size(int64_t off_in,
                                int64_t ser_block_size_in,
                                int64_t extent_size,
                                const std::vector<uint32_t> &boundaries,
                                int64_t *offset_out, int64_t *size_out) {
    int64_t offset;
    int64_t end_offset;
    read_ahead_interval(off_in, ser_block_size_in, extent_size,
                        APPROXIMATE_READ_AHEAD_SIZE,
                        DEVICE_BLOCK_SIZE,
                        boundaries,
                        &offset,
                        &end_offset);

    *offset_out = offset;
    *size_out = end_offset - offset;
}

class dbm_read_ahead_t {
public:
    static std::vector<uint32_t> get_boundaries(data_block_manager_t *parent,
                                                int64_t off_in) {
        const gc_entry_t *entry
            = parent->entries.get(off_in / parent->static_config->extent_size());
        guarantee(entry != NULL);

        return entry->block_boundaries();
    }

    static void perform_read_ahead(data_block_manager_t *const parent,
                                   const int64_t off_in,
                                   const uint32_t ser_block_size_in,
                                   void *const buf_out,
                                   file_account_t *const io_account) {
        const std::vector<uint32_t> boundaries = get_boundaries(parent, off_in);

        int64_t read_ahead_offset;
        int64_t read_ahead_size;

        // Finish initialization.
        read_ahead_offset_and_size(off_in,
                                   ser_block_size_in,
                                   parent->static_config->extent_size(),
                                   boundaries,
                                   &read_ahead_offset,
                                   &read_ahead_size);

        scoped_malloc_t<char> read_ahead_buf(
                malloc_aligned(read_ahead_size, DEVICE_BLOCK_SIZE));

        // Do the disk read!
        co_read(parent->dbfile, read_ahead_offset, read_ahead_size,
                read_ahead_buf.get(), io_account);

        // Walk over the read ahead buffer and copy stuff...
        int64_t extent_offset = floor_aligned(read_ahead_offset,
                                              parent->static_config->extent_size());
        uint32_t relative_offset = read_ahead_offset - extent_offset;
        uint32_t relative_end_offset
            = (read_ahead_offset + read_ahead_size) - extent_offset;

        auto lower_it = std::lower_bound(boundaries.begin(), boundaries.end(),
                                         relative_offset);
        auto const upper_it = std::upper_bound(boundaries.begin(), boundaries.end(),
                                               relative_end_offset) - 1;

        bool handled_required_block = false;

        for (; lower_it < upper_it; ++lower_it) {
            const char *current_buf
                = read_ahead_buf.get() + (*lower_it - relative_offset);

            int64_t current_offset = *lower_it + extent_offset;

            if (current_offset == off_in) {
                guarantee(!handled_required_block);

                memcpy(buf_out, current_buf, ser_block_size_in);
                handled_required_block = true;
            } else {
                // RSI: Remove block id from block metadata, just put a
                // reverse-lookup table in the gc entries, initialized from the LBA.
                const block_id_t block_id
                    = reinterpret_cast<const ls_buf_data_t *>(current_buf)->block_id;

                const index_block_info_t info
                    = parent->serializer->lba_index->get_block_info(block_id);

                const bool block_is_live = info.offset.has_value() &&
                    current_offset == info.offset.get_value();

                if (!block_is_live) {
                    continue;
                }

                scoped_malloc_t<ser_buffer_t> data = parent->serializer->malloc();
                memcpy(data.get(), current_buf, info.ser_block_size);
                guarantee(info.ser_block_size <= *(lower_it + 1) - *lower_it);

                counted_t<ls_block_token_pointee_t> ls_token
                    = parent->serializer->generate_block_token(current_offset,
                                                               info.ser_block_size);

                counted_t<standard_block_token_t> token
                    = to_standard_block_token(block_id, ls_token);

                parent->serializer->offer_buf_to_read_ahead_callbacks(
                        block_id,
                        std::move(data),
                        block_size_t::unsafe_make(info.ser_block_size),
                        token,
                        info.recency);
            }
        }

        guarantee(handled_required_block);
    }
};


bool data_block_manager_t::should_perform_read_ahead(int64_t offset) {
    unsigned int extent_id = static_config->extent_index(offset);

    gc_entry_t *entry = entries.get(extent_id);

    // If the extent was written, we don't perform read ahead because it would
    // a) be potentially useless and b) has an elevated risk of conflicting with
    // active writes on the io queue.
    return !entry->was_written && serializer->should_perform_read_ahead();
}

void data_block_manager_t::read(int64_t off_in, uint32_t ser_block_size_in,
                                void *buf_out, file_account_t *io_account) {
    guarantee(state == state_ready);
    if (should_perform_read_ahead(off_in)) {
        dbm_read_ahead_t::perform_read_ahead(this, off_in, ser_block_size_in,
                                             buf_out, io_account);
    } else {
        if (divides(DEVICE_BLOCK_SIZE, reinterpret_cast<intptr_t>(buf_out)) &&
            divides(DEVICE_BLOCK_SIZE, off_in) &&
            divides(DEVICE_BLOCK_SIZE, ser_block_size_in)) {
            co_read(dbfile, off_in, ser_block_size_in, buf_out, io_account);
        } else {
            int64_t floor_off_in = floor_aligned(off_in, DEVICE_BLOCK_SIZE);
            int64_t ceil_off_end = ceil_aligned(off_in + ser_block_size_in,
                                                DEVICE_BLOCK_SIZE);
            scoped_malloc_t<char> buf(malloc_aligned(ceil_off_end - floor_off_in,
                                                     DEVICE_BLOCK_SIZE));
            co_read(dbfile, floor_off_in, ceil_off_end - floor_off_in,
                    buf.get(), io_account);

            memcpy(buf_out, buf.get() + (off_in - floor_off_in), ser_block_size_in);
        }
    }
}

std::vector<counted_t<ls_block_token_pointee_t> >
data_block_manager_t::many_writes(const std::vector<buf_write_info_t> &writes,
                                  bool assign_new_block_sequence_id,
                                  file_account_t *io_account,
                                  iocallback_t *cb) {
    // Either we're ready to write, or we're shutting down and just finished reading
    // blocks for gc and called do_write.
    guarantee(state == state_ready ||
              (state == state_shutting_down && gc_state.step() == gc_write));

    // These tokens are grouped by extent.  You can do a contiguous write in each
    // extent.
    std::vector<std::vector<counted_t<ls_block_token_pointee_t> > > token_groups
        = gimme_some_new_offsets(writes);

    for (auto it = writes.begin(); it != writes.end(); ++it) {
        it->buf->ser_header.block_id = it->block_id;
        if (assign_new_block_sequence_id) {
            ++serializer->latest_block_sequence_id;
            it->buf->ser_header.block_sequence_id = serializer->latest_block_sequence_id;
        }
    }

    stats->pm_serializer_data_blocks_written += writes.size();

    struct intermediate_cb_t : public iocallback_t {
        virtual void on_io_complete() {
            --ops_remaining;
            if (ops_remaining == 0) {
                iocallback_t *local_cb = cb;
                delete this;
                local_cb->on_io_complete();
            }
        }

        size_t ops_remaining;
        scoped_array_t<scoped_malloc_t<char> > bufs;
        iocallback_t *cb;
    };

    intermediate_cb_t *const intermediate_cb = new intermediate_cb_t;
    intermediate_cb->ops_remaining = token_groups.size();
    intermediate_cb->bufs.init(token_groups.size());
    intermediate_cb->cb = cb;

    size_t write_number = 0;
    for (size_t i = 0; i < token_groups.size(); ++i) {
        const int64_t front_offset = token_groups[i].front()->offset();
        const int64_t back_offset = token_groups[i].back()->offset()
            + token_groups[i].back()->ser_block_size();

        guarantee(divides(DEVICE_BLOCK_SIZE, front_offset));

        const int64_t write_size = ceil_aligned(back_offset - front_offset,
                                                DEVICE_BLOCK_SIZE);

        scoped_malloc_t<char> buf(malloc_aligned(write_size, DEVICE_BLOCK_SIZE));

        // Used to zero out unused portions of the buf, so that we don't write
        // undefined data (arbitrary contents of memory) to disk.
        int64_t last_written_offset = front_offset;

        for (auto it = token_groups[i].begin(); it != token_groups[i].end(); ++it) {
            const int64_t it_offset = (*it)->offset();
            const uint32_t it_ser_block_size = (*it)->ser_block_size();
            guarantee(it_offset >= last_written_offset);

            // The behavior of gimme_some_new_offsets is supposed to retain order, so
            // we expect writes[write_number] to have the currently-relevant write.
            guarantee(writes[write_number].ser_block_size == it_ser_block_size);

            memset(buf.get() + (last_written_offset - front_offset), 0,
                   it_offset - last_written_offset);
            memcpy(buf.get() + (it_offset - front_offset), writes[write_number].buf,
                   it_ser_block_size);

            last_written_offset = it_offset + it_ser_block_size;

            ++write_number;
        }

        memset(buf.get() + (last_written_offset - front_offset), 0,
               write_size - (last_written_offset - front_offset));

        dbfile->write_async(front_offset, back_offset - front_offset,
                            buf.get(), io_account, intermediate_cb, file_t::NO_DATASYNCS);

        intermediate_cb->bufs[i] = std::move(buf);
    }

    std::vector<counted_t<ls_block_token_pointee_t> > ret;
    ret.reserve(writes.size());
    for (auto it = token_groups.begin(); it != token_groups.end(); ++it) {
        for (auto jt = it->begin(); jt != it->end(); ++jt) {
            ret.push_back(std::move(*jt));
        }
    }

    return ret;
}

void data_block_manager_t::check_and_handle_empty_extent(unsigned int extent_id) {
    gc_entry_t *entry = entries.get(extent_id);
    if (!entry) {
        return; // The extent has already been deleted
    }

    if (entry->all_garbage() && entry->state != gc_entry_t::state_active) {
        /* Every block in the extent is now garbage. */
        switch (entry->state) {
            case gc_entry_t::state_reconstructing:
                unreachable("Marking something as garbage during startup.");

            case gc_entry_t::state_active:
                unreachable("We shouldn't have gotten here.");

            /* Remove from the young extent queue */
            case gc_entry_t::state_young:
                young_extent_queue.remove(entry);
                break;

            /* Remove from the priority queue */
            case gc_entry_t::state_old:
                gc_pq.remove(entry->our_pq_entry);
                gc_stats.old_total_block_bytes -= static_config->extent_size();
                gc_stats.old_garbage_block_bytes -= static_config->extent_size();
                break;

            /* Notify the GC that the extent got released during GC */
            case gc_entry_t::state_in_gc:
                guarantee(gc_state.current_entry == entry);
                gc_state.current_entry = NULL;
                break;
            default:
                unreachable();
        }

        ++stats->pm_serializer_data_extents_reclaimed;
        entry->destroy();
        rassert(entries.get(extent_id) == NULL);

    } else if (entry->state == gc_entry_t::state_old) {
        entry->our_pq_entry->update();
    }
}

file_account_t *data_block_manager_t::choose_gc_io_account() {
    // Start going into high priority as soon as the garbage ratio is more than
    // 2% above the configured goal.
    // The idea is that we use the nice i/o account whenever possible, except
    // if it proves insufficient to maintain an acceptable garbage ratio, in
    // which case we switch over to the high priority account until the situation
    // has improved.

    // This means that we can end up oscillating between both accounts, which
    // is probably fine. TODO: Make sure it actually is in practice!
    if (garbage_ratio() > dynamic_config->gc_high_ratio * 1.02) {
        return gc_io_account_high.get();
    } else {
        return gc_io_account_nice.get();
    }
}

void data_block_manager_t::mark_garbage(int64_t offset, extent_transaction_t *txn) {
    unsigned int extent_id = static_config->extent_index(offset);
    gc_entry_t *entry = entries.get(extent_id);
    unsigned int block_index = entry->block_index(offset);

    // Now we set the i_array entry to zero.  We make an extra reference to the
    // extent which gets held until we commit the transaction.
    txn->push_extent(extent_manager->copy_extent_reference(entry->extent_ref));

    guarantee(!entry->block_is_garbage(block_index));
    entry->mark_garbage_indexwise(block_index);

    // Add to old garbage count if necessary (works because of the
    // !entry->block_is_garbage(block_index) assertion above).
    if (entry->state == gc_entry_t::state_old && entry->block_is_garbage(block_index)) {
        gc_stats.old_garbage_block_bytes += entry->block_size(block_index);
    }

    check_and_handle_empty_extent(extent_id);
}

void data_block_manager_t::mark_live_tokenwise(int64_t offset) {
    unsigned int extent_id = static_config->extent_index(offset);
    gc_entry_t *entry = entries.get(extent_id);
    rassert(entry != NULL);
    unsigned int block_index = entry->block_index(offset);

    entry->mark_live_tokenwise(block_index);
}

void data_block_manager_t::mark_garbage_tokenwise(int64_t offset) {
    unsigned int extent_id = static_config->extent_index(offset);
    gc_entry_t *entry = entries.get(extent_id);

    rassert(entry != NULL);

    unsigned int block_index = entry->block_index(offset);

    guarantee(!entry->block_is_garbage(block_index));

    entry->mark_garbage_tokenwise(block_index);

    // Add to old garbage count if necessary (works because of the
    // !entry->block_is_garbage(block_index) assertion above).
    if (entry->state == gc_entry_t::state_old && entry->block_is_garbage(block_index)) {
        gc_stats.old_garbage_block_bytes += entry->block_size(block_index);
    }

    check_and_handle_empty_extent(extent_id);
}

void data_block_manager_t::start_gc() {
    if (gc_state.step() == gc_ready) run_gc();
}

data_block_manager_t::gc_writer_t::gc_writer_t(gc_write_t *writes, int num_writes, data_block_manager_t *_parent)
    : done(num_writes == 0), parent(_parent) {
    if (!done) {
        coro_t::spawn(boost::bind(&data_block_manager_t::gc_writer_t::write_gcs, this, writes, num_writes));
    }
}

struct block_write_cond_t : public cond_t, public iocallback_t {
    void on_io_complete() {
        pulse();
    }
};

// RSI: Make num_writes be a size_t.
void data_block_manager_t::gc_writer_t::write_gcs(gc_write_t *writes, int num_writes) {
    if (parent->gc_state.current_entry != NULL) {
        block_write_cond_t block_write_cond;

        // We acquire block tokens for all the blocks before writing new
        // version.  The point of this is to make sure the _new_ block is
        // correctly "alive" when we write it.

        std::vector<counted_t<ls_block_token_pointee_t> > old_block_tokens;
        old_block_tokens.reserve(num_writes);

        // New block tokens, to hold the return value of
        // data_block_manager_t::write() instead of immediately discarding the
        // created token and causing the extent or block to be collected.
        std::vector<counted_t<ls_block_token_pointee_t> > new_block_tokens;

        {
            // Step 1: Write buffers to disk and assemble index operations
            ASSERT_NO_CORO_WAITING;

            std::vector<buf_write_info_t> the_writes;
            the_writes.reserve(num_writes);
            for (int i = 0; i < num_writes; ++i) {
                old_block_tokens.push_back(parent->serializer->generate_block_token(writes[i].old_offset,
                                                                                    writes[i].ser_block_size));

                the_writes.push_back(buf_write_info_t(writes[i].buf,
                                                      writes[i].ser_block_size,
                                                      writes[i].buf->ser_header.block_id));
            }

            new_block_tokens
                = parent->many_writes(the_writes, false, parent->choose_gc_io_account(),
                                      &block_write_cond);

            guarantee(new_block_tokens.size() == static_cast<size_t>(num_writes));

            for (int i = 0; i < num_writes; ++i) {
                // RSI: Is setting writes[i].new_offset the right way to pass this information along?
                writes[i].new_offset = new_block_tokens[i]->offset();
            }
        }

        // Step 2: Wait on all writes to finish
        block_write_cond.wait();

        // We created block tokens for our blocks we're writing, so
        // there's no way the current entry could have become NULL.
        guarantee(parent->gc_state.current_entry != NULL);

        std::vector<index_write_op_t> index_write_ops;

        // Step 3: Figure out index ops.  It's important that we do this
        // now, right before the index_write, so that the updates to the
        // index are done atomically.
        {
            ASSERT_NO_CORO_WAITING;

            for (int i = 0; i < num_writes; ++i) {
                unsigned int block_index
                    = parent->gc_state.current_entry->block_index(writes[i].old_offset);

                if (parent->gc_state.current_entry->block_referenced_by_index(block_index)) {
                    block_id_t block_id = writes[i].buf->ser_header.block_id;

                    index_write_ops.push_back(
                            index_write_op_t(block_id,
                                             to_standard_block_token(
                                                     block_id,
                                                     new_block_tokens[i])));
                }

                // (If we don't have an i_array entry, the block is referenced
                // by a non-negative number of tokens only.  These get tokens
                // remapped later.)
            }

            // Step 4A: Remap tokens to new offsets.  It is important
            // that we do this _before_ calling index_write.
            // Otherwise, the token_offset map would still point to
            // the extent we're gcing.  Then somebody could do an
            // index_write after our index_write starts but before it
            // returns in Step 4 below, resulting in i_array entries
            // that point to the current entry.  This should empty out
            // all the t_array bits.
            for (int i = 0; i < num_writes; ++i) {
                parent->serializer->remap_block_to_new_offset(writes[i].old_offset, writes[i].new_offset);
            }

            // Step 4A-2: Now that the block tokens have been remapped
            // to a new offset, destroying these tokens will update
            // the bits in the t_array of the new offset (if they're
            // the last token).
            old_block_tokens.clear();
            new_block_tokens.clear();
        }

        // Step 4B: Commit the transaction to the serializer, emptying
        // out all the i_array bits.
        parent->serializer->index_write(index_write_ops, parent->choose_gc_io_account());

        ASSERT_NO_CORO_WAITING;

        index_write_ops.clear();
    }

    {
        ASSERT_NO_CORO_WAITING;

        // Step 5: Call parent
        done = true;
        parent->on_gc_write_done();
    }

    // Step 6: Delete us
    delete this;
}

void data_block_manager_t::on_gc_write_done() {

    // Continue GC
    run_gc();
}

void data_block_manager_t::run_gc() {
    bool run_again = true;
    while (run_again) {
        run_again = false;
        switch (gc_state.step()) {
            case gc_ready: {
                if (gc_pq.empty() || !should_we_keep_gcing(*gc_pq.peak())) {
                    return;
                }

                ASSERT_NO_CORO_WAITING;

                ++stats->pm_serializer_data_extents_gced;

                /* grab the entry */
                gc_state.current_entry = gc_pq.pop();
                gc_state.current_entry->our_pq_entry = NULL;

                guarantee(gc_state.current_entry->state == gc_entry_t::state_old);
                gc_state.current_entry->state = gc_entry_t::state_in_gc;
                gc_stats.old_garbage_block_bytes -= gc_state.current_entry->garbage_bytes();
                gc_stats.old_total_block_bytes -= static_config->extent_size();

                /* read all the live data into buffers */

                /* make sure the read callback knows who we are */
                gc_state.gc_read_callback.parent = this;

                guarantee(gc_state.refcount == 0);

                // read_async can call the callback immediately, which
                // will then decrement the refcount (and that's all it
                // will do if the refcount is not zero).  So we
                // increment the refcount here and set the step to
                // gc_read, before any calls to read_async.
                gc_state.refcount++;

                guarantee(!gc_state.gc_blocks.has());
                gc_state.gc_blocks.init(malloc_aligned(extent_manager->extent_size,
                                                       DEVICE_BLOCK_SIZE));
                gc_state.set_step(gc_read);

                // We're going to send as few discrete reads as possible, minimizing
                // disk->CPU bandwidth usage, instead of simply reading the entire
                // extent.

                uint32_t current_interval_begin = 0;
                uint32_t current_interval_end = 0;

                const int64_t extent_offset = gc_state.current_entry->extent_ref.offset();

                for (unsigned int i = 0, bpe = gc_state.current_entry->num_blocks();
                     i < bpe;
                     ++i) {
                    if (!gc_state.current_entry->block_is_garbage(i)) {
                        const uint32_t beg
                            = floor_aligned(gc_state.current_entry->relative_offset(i),
                                            DEVICE_BLOCK_SIZE);
                        const uint32_t end
                            = ceil_aligned(gc_state.current_entry->relative_offset(i)
                                           + gc_state.current_entry->block_size(i),
                                           DEVICE_BLOCK_SIZE);

                        if (beg <= current_interval_end) {
                            current_interval_end = end;
                        } else {
                            if (current_interval_end > current_interval_begin) {
                                gc_state.refcount++;
                                dbfile->read_async(
                                        extent_offset + current_interval_begin,
                                        current_interval_end - current_interval_begin,
                                        gc_state.gc_blocks.get() + current_interval_begin,
                                        choose_gc_io_account(),
                                        &gc_state.gc_read_callback);
                            }

                            current_interval_begin = beg;
                            current_interval_end = end;
                        }
                    }
                }

                guarantee(current_interval_begin < current_interval_end);

                gc_state.refcount++;
                dbfile->read_async(
                        extent_offset + current_interval_begin,
                        current_interval_end - current_interval_begin,
                        gc_state.gc_blocks.get() + current_interval_begin,
                        choose_gc_io_account(),
                        &gc_state.gc_read_callback);

                // Fall through to the gc_read case, where we
                // decrement the refcount we incremented before the
                // for loop.
            }
            case gc_read: {
                gc_state.refcount--;
                if (gc_state.refcount > 0) {
                    /* We got a block, but there are still more to go */
                    break;
                }

                /* If other forces cause all of the blocks in the extent to become
                garbage before we even finish GCing it, they will set current_entry
                to NULL. */
                if (gc_state.current_entry == NULL) {
                    guarantee(gc_state.gc_blocks.has());
                    gc_state.gc_blocks.reset();
                    gc_state.set_step(gc_ready);
                    break;
                }

                /* an array to put our writes in */
                int num_writes = gc_state.current_entry->num_live_blocks();

                gc_writes.clear();
                for (unsigned int i = 0, iend = gc_state.current_entry->num_blocks(); i < iend; ++i) {

                    /* We re-check the bit array here in case a write came in for one
                    of the blocks we are GCing. We wouldn't want to overwrite the new
                    valid data with out-of-date data. */
                    if (gc_state.current_entry->block_is_garbage(i)) {
                        continue;
                    }

                    ser_buffer_t *block = reinterpret_cast<ser_buffer_t *>(gc_state.gc_blocks.get() + gc_state.current_entry->relative_offset(i));
                    const int64_t block_offset = gc_state.current_entry->extent_ref.offset()
                        + gc_state.current_entry->relative_offset(i);

                    block_id_t id;
                    // The block is either referenced by an index or by a token (or both)
                    if (gc_state.current_entry->block_referenced_by_index(i)) {
                        id = block->ser_header.block_id;
                        guarantee(id != NULL_BLOCK_ID);
                    } else {
                        id = NULL_BLOCK_ID;
                    }

                    gc_writes.push_back(gc_write_t(id, block, block_offset,
                                                   gc_state.current_entry->block_size(i)));
                }

                guarantee(gc_writes.size() == static_cast<size_t>(num_writes));

                gc_state.set_step(gc_write);

                /* schedule the write */
                gc_writer_t *gc_writer = new gc_writer_t(gc_writes.data(), gc_writes.size(), this);
                if (!gc_writer->done) {
                    break;
                } else {
                    delete gc_writer;
                }
            }

            case gc_write:
                //We need to do this here so that we don't
                //get stuck on the GC treadmill
                mark_unyoung_entries();
                /* Our write should have forced all of the blocks in the extent to
                become garbage, which should have caused the extent to be released
                and gc_state.current_entry to become NULL. */

                guarantee(gc_state.current_entry == NULL,
                          "%p: %zd garbage bytes left on the extent, %zd index-referenced "
                          "bytes, %zd token-referenced bytes, at offset %" PRIi64
                          ".  block dump:\n%s\n",
                          this,
                          gc_state.current_entry->garbage_bytes(),
                          gc_state.current_entry->index_bytes(),
                          gc_state.current_entry->token_bytes(),
                          gc_state.current_entry->extent_ref.offset(),
                          gc_state.current_entry->format_block_infos("\n").c_str());

                guarantee(gc_state.refcount == 0);

                guarantee(gc_state.gc_blocks.has());
                gc_state.gc_blocks.reset();
                gc_state.set_step(gc_ready);

                if (state == state_shutting_down) {
                    actually_shutdown();
                    return;
                }

                run_again = true;   // We might want to start another GC round
                break;

            case gc_reconstruct:
            default:
                unreachable("Unknown gc_step");
        }
    }
}

void data_block_manager_t::prepare_metablock(metablock_mixin_t *metablock) {
    guarantee(state == state_ready || state == state_shutting_down);

    if (active_extent != NULL) {
        metablock->active_extent = active_extent->extent_ref.offset();
        metablock->blocks_in_active_extent = active_extent->num_blocks();
    } else {
        metablock->active_extent = NULL_OFFSET;
        metablock->blocks_in_active_extent = 0;
    }
}

bool data_block_manager_t::shutdown(shutdown_callback_t *cb) {
    rassert(cb != NULL);
    guarantee(state == state_ready);
    state = state_shutting_down;

    if (gc_state.step() != gc_ready) {
        shutdown_callback = cb;
        return false;
    } else {
        shutdown_callback = NULL;
        actually_shutdown();
        return true;
    }
}

void data_block_manager_t::actually_shutdown() {
    guarantee(state == state_shutting_down);
    state = state_shut_down;

    guarantee(reconstructed_extents.head() == NULL);

    if (active_extent != NULL) {
        UNUSED int64_t extent = active_extent->extent_ref.release();
        delete active_extent;
        active_extent = NULL;
    }

    while (gc_entry_t *entry = young_extent_queue.head()) {
        young_extent_queue.remove(entry);
        UNUSED int64_t extent = entry->extent_ref.release();
        delete entry;
    }

    while (!gc_pq.empty()) {
        gc_entry_t *entry = gc_pq.pop();
        UNUSED int64_t extent = entry->extent_ref.release();
        delete entry;
    }

    if (shutdown_callback) {
        shutdown_callback->on_datablock_manager_shutdown();
    }
}

std::vector<std::vector<counted_t<ls_block_token_pointee_t> > >
data_block_manager_t::gimme_some_new_offsets(const std::vector<buf_write_info_t> &writes) {
    ASSERT_NO_CORO_WAITING;

    // Start a new extent if necessary.
    if (active_extent == NULL) {
        active_extent = new gc_entry_t(this);
        ++stats->pm_serializer_data_extents_allocated;
    }


    // RSI: Make sure that file async write calls and read calls use spawn_ordered
    // or whatever order-preserving cross-thread communication is necessary.  SIGH.

    // RSI: Add a unit test on a real file with a huuuge number of writes.
    guarantee(active_extent->state == gc_entry_t::state_active);

    bool align_to_device_block_size = true;

    std::vector<std::vector<counted_t<ls_block_token_pointee_t> > > ret;

    std::vector<counted_t<ls_block_token_pointee_t> > tokens;
    for (auto it = writes.begin(); it != writes.end(); ++it) {
        uint32_t relative_offset;
        unsigned int block_index;
        if (!active_extent->new_offset(it->ser_block_size, align_to_device_block_size,
                                       &relative_offset, &block_index)) {
            // Move the active_extent gc_entry_t to the young extent queue, and make a
            // new gc_entry_t.
            active_extent->state = gc_entry_t::state_young;
            young_extent_queue.push_back(active_extent);
            mark_unyoung_entries();

            active_extent = new gc_entry_t(this);
            ++stats->pm_serializer_data_extents_allocated;
            const bool succeeded = active_extent->new_offset(it->ser_block_size,
                                                             true,
                                                             &relative_offset,
                                                             &block_index);
            guarantee(succeeded);

            // Push the current group of tokens, if it's nonempty, onto the return vector.
            if (!tokens.empty()) {
                ret.push_back(std::move(tokens));
                tokens.clear();
            }
        }

        align_to_device_block_size = false;

        const int64_t offset = active_extent->extent_ref.offset() + relative_offset;
        active_extent->was_written = true;
        active_extent->mark_live_tokenwise(block_index);

        tokens.push_back(serializer->generate_block_token(offset, it->ser_block_size));
    }

    if (!tokens.empty()) {
        ret.push_back(std::move(tokens));
    }

    return ret;
}

// Looks at young_extent_queue and pops things off the queue that are
// no longer deemed young, putting them on the priority queue.
void data_block_manager_t::mark_unyoung_entries() {
    ASSERT_NO_CORO_WAITING;
    while (young_extent_queue.size() > GC_YOUNG_EXTENT_MAX_SIZE) {
        remove_last_unyoung_entry();
    }

    uint64_t current_time = current_microtime();

    while (young_extent_queue.head()
           && current_time - young_extent_queue.head()->timestamp > GC_YOUNG_EXTENT_TIMELIMIT_MICROS) {
        remove_last_unyoung_entry();
    }
}

// Pops young_extent_queue and puts it on the priority queue.
// Assumes young_extent_queue is not empty.
void data_block_manager_t::remove_last_unyoung_entry() {
    ASSERT_NO_CORO_WAITING;
    gc_entry_t *entry = young_extent_queue.head();
    young_extent_queue.remove(entry);

    guarantee(entry->state == gc_entry_t::state_young);
    entry->state = gc_entry_t::state_old;

    entry->our_pq_entry = gc_pq.push(entry);

    gc_stats.old_total_block_bytes += static_config->extent_size();
    gc_stats.old_garbage_block_bytes += entry->garbage_bytes();
}

void gc_entry_t::add_self_to_parent_entries() {
    unsigned int extent_id = parent->static_config->extent_index(extent_offset);
    guarantee(parent->entries.get(extent_id) == NULL);
    parent->entries.set(extent_id, this);

    ++parent->stats->pm_serializer_data_extents;
}

// Constructs a new active extent.
gc_entry_t::gc_entry_t(data_block_manager_t *_parent)
    : parent(_parent),
      extent_ref(parent->extent_manager->gen_extent()),
      timestamp(current_microtime()),
      was_written(false),
      state(state_active),
      extent_offset(extent_ref.offset()) {
    add_self_to_parent_entries();
}

// Constructor for a mid-reconstructing extent.
gc_entry_t::gc_entry_t(data_block_manager_t *_parent, int64_t _offset)
    : parent(_parent),
      extent_ref(parent->extent_manager->reserve_extent(_offset)),
      timestamp(current_microtime()),
      was_written(false),
      state(state_reconstructing),
      extent_offset(extent_ref.offset()) {
    add_self_to_parent_entries();
}

gc_entry_t::~gc_entry_t() {
    unsigned int extent_id = parent->static_config->extent_index(extent_offset);
    guarantee(parent->entries.get(extent_id) == this);
    parent->entries.set(extent_id, NULL);

    --parent->stats->pm_serializer_data_extents;
}

uint32_t gc_entry_t::back_relative_offset() const {
    return block_infos.empty()
        ? 0
        : block_infos.back().relative_offset + block_infos.back().ser_block_size;
}

std::vector<uint32_t> gc_entry_t::block_boundaries() const {
    guarantee(state != state_reconstructing);

    std::vector<uint32_t> ret;
    for (auto it = block_infos.begin(); it != block_infos.end(); ++it) {
        ret.push_back(it->relative_offset);
    }

    ret.push_back(back_relative_offset());
    return ret;
}

uint32_t gc_entry_t::block_size(unsigned int block_index) const {
    guarantee(state != state_reconstructing);
    guarantee(block_index < block_infos.size());
    return block_infos[block_index].ser_block_size;
}

uint32_t gc_entry_t::relative_offset(unsigned int block_index) const {
    guarantee(state != state_reconstructing);
    guarantee(block_index < block_infos.size());
    return block_infos[block_index].relative_offset;
}

unsigned int gc_entry_t::block_index(const int64_t offset) const {
    guarantee(state != state_reconstructing);
    guarantee(offset >= extent_ref.offset());
    guarantee(offset < extent_ref.offset() + UINT32_MAX);
    const uint32_t relative_offset = offset - extent_ref.offset();

    for (unsigned int i = 0; i < block_infos.size(); ++i) {
        if (block_infos[i].relative_offset == relative_offset) {
            return i;
        }
    }

    crash("block_index requested for invalid offset (offset = %ld)", offset);
}

bool gc_entry_t::new_offset(uint32_t ser_block_size,
                            bool align_to_device_block_size,
                            uint32_t *relative_offset_out,
                            unsigned int *block_index_out) {
    // Returns true if there's enough room at the end of the extent for the new
    // block.
    guarantee(state == state_active);
    guarantee(ser_block_size <= parent->static_config->extent_size());

    uint32_t offset = align_to_device_block_size
        ? ceil_aligned(back_relative_offset(), DEVICE_BLOCK_SIZE)
        : back_relative_offset();
    guarantee(offset <= parent->static_config->extent_size());

    if (offset > parent->static_config->extent_size() - ser_block_size) {
        return false;
    } else {
        *relative_offset_out = offset;
        *block_index_out = block_infos.size();
        block_infos.push_back(block_info_t{offset, ser_block_size, false, false});
        return true;
    }
}

void gc_entry_t::make_active() {
    guarantee(state == state_reconstructing);
    state = state_active;
}

void gc_entry_t::destroy() {
    parent->extent_manager->release_extent(std::move(extent_ref));
    delete this;
}

uint64_t gc_entry_t::garbage_bytes() const {
    uint64_t b = parent->static_config->extent_size();
    for (auto it = block_infos.begin(); it < block_infos.end(); ++it) {
        if (it->token_referenced || it->index_referenced) {
            // RSI: Support writing blocks packed tighter than DEVICE_BLOCK_SIZE.
            b -= ceil_aligned(it->ser_block_size, DEVICE_BLOCK_SIZE);
        }
    }
    return b;
}

uint64_t gc_entry_t::token_bytes() const {
    uint64_t b = 0;
    for (auto it = block_infos.begin(); it < block_infos.end(); ++it) {
        if (it->token_referenced) {
            // RSI: Support writing blocks packed tighter than DEVICE_BLOCK_SIZE.
            b += ceil_aligned(it->ser_block_size, DEVICE_BLOCK_SIZE);
        }
    }
    return b;
}

uint64_t gc_entry_t::index_bytes() const {
    uint64_t b = 0;
    for (auto it = block_infos.begin(); it < block_infos.end(); ++it) {
        if (it->index_referenced) {
            // RSI: Support writing blocks packed tighter than DEVICE_BLOCK_SIZE.
            b += ceil_aligned(it->ser_block_size, DEVICE_BLOCK_SIZE);
        }
    }
    return b;
}

void gc_entry_t::mark_live_indexwise_with_offset(int64_t offset,
                                                 uint32_t ser_block_size) {
    guarantee(offset >= extent_ref.offset() && offset < extent_ref.offset() + UINT32_MAX);

    uint32_t relative_offset = offset - extent_ref.offset();

    uint32_t last_back_offset = 0;
    auto it = block_infos.begin();
    while (it != block_infos.end()) {
        if (it->relative_offset > relative_offset) {
            guarantee(it->relative_offset >= relative_offset + ser_block_size);
            block_infos.insert(it, block_info_t{relative_offset, ser_block_size, false, true});
            return;
        } else if (it->relative_offset == relative_offset) {
            guarantee(it->ser_block_size == ser_block_size);
            it->index_referenced = true;
            return;
        } else {
            last_back_offset = it->relative_offset + it->ser_block_size;
            guarantee(last_back_offset <= relative_offset);
            ++it;
        }
    }

    // We reached block_infos.end(), still haven't inserted our block!
    // We already asserted that last_back_offset <= relative_offset.
    block_infos.insert(it, block_info_t{relative_offset, ser_block_size, false, true});
}

std::string gc_entry_t::format_block_infos(const char *separator) const {
    const int64_t offset = extent_ref.offset();
    std::string ret;
    for (auto it = block_infos.begin(); it != block_infos.end(); ++it) {
        ret += strprintf("%s[%" PRIi64 "..+%" PRIu32 ") %c%c",
                         it == block_infos.begin() ? "" : separator,
                         offset + it->relative_offset, it->ser_block_size,
                         it->token_referenced ? 'T' : ' ',
                         it->index_referenced ? 'I' : ' ');
    }
    return ret;
}

/* functions for gc structures */

// Answers the following question: We're in the middle of gc'ing, and
// look, it's the next largest entry.  Should we keep gc'ing?  Returns
// false when the entry is active or young, or when its garbage ratio
// is lower than GC_THRESHOLD_RATIO_*.
bool data_block_manager_t::should_we_keep_gcing(UNUSED const gc_entry_t& entry) const {
    return !gc_state.should_be_stopped && garbage_ratio() > dynamic_config->gc_low_ratio;
}

// Answers the following question: Do we want to bother gc'ing?
// Returns true when our garbage_ratio is greater than
// GC_THRESHOLD_RATIO_*.
bool data_block_manager_t::do_we_want_to_start_gcing() const {
    return !gc_state.should_be_stopped && garbage_ratio() > dynamic_config->gc_high_ratio;
}

bool gc_entry_less_t::operator()(const gc_entry_t *x, const gc_entry_t *y) {
    return x->garbage_bytes() < y->garbage_bytes();
}

/****************
 *Stat functions*
 ****************/

double data_block_manager_t::garbage_ratio() const {
    if (gc_stats.old_total_block_bytes.get() == 0) {
        return 0.0;
    } else {
        double old_garbage = gc_stats.old_garbage_block_bytes.get();
        double old_total = gc_stats.old_total_block_bytes.get();
        return old_garbage / (old_total + extent_manager->held_extents() * static_config->extent_size());
    }
}

bool data_block_manager_t::disable_gc(gc_disable_callback_t *cb) {
    // We _always_ call the callback!

    guarantee(gc_state.gc_disable_callback == NULL);
    gc_state.should_be_stopped = true;

    if (gc_state.step() != gc_ready && gc_state.step() != gc_reconstruct) {
        gc_state.gc_disable_callback = cb;
        return false;
    } else {
        cb->on_gc_disabled();
        return true;
    }
}

void data_block_manager_t::enable_gc() {
    gc_state.should_be_stopped = false;
}

void data_block_manager_t::gc_stat_t::operator+=(int64_t num) {
    val += num;
    *perfmon += num;
}

void data_block_manager_t::gc_stat_t::operator-=(int64_t num) {
    val -= num;
    *perfmon -= num;
}

data_block_manager_t::gc_stats_t::gc_stats_t(log_serializer_stats_t *_stats)
    : old_total_block_bytes(&_stats->pm_serializer_old_total_block_bytes),
      old_garbage_block_bytes(&_stats->pm_serializer_old_garbage_block_bytes) { }
