// Copyright 2010-2014 RethinkDB, all rights reserved.
#include "unittest/gtest.hpp"

#include "arch/io/disk.hpp"
#include "btree/btree_store.hpp"
#include "btree/operations.hpp"
#include "buffer_cache/alt/alt.hpp"
#include "buffer_cache/alt/blob.hpp"
#include "unittest/unittest_utils.hpp"
#include "rdb_protocol/btree.hpp"
#include "rdb_protocol/protocol.hpp"
#include "serializer/config.hpp"

namespace unittest {

void run_sindex_low_level_operations_test() {
    temp_file_t temp_file;

    io_backender_t io_backender(file_direct_io_mode_t::buffered_desired);

    filepath_file_opener_t file_opener(temp_file.name(), &io_backender);
    standard_serializer_t::create(
        &file_opener,
        standard_serializer_t::static_config_t());

    standard_serializer_t serializer(
        standard_serializer_t::dynamic_config_t(),
        &file_opener,
        &get_global_perfmon_collection());

    cache_t cache(&serializer, alt_cache_config_t(),
                  &get_global_perfmon_collection());
    cache_conn_t cache_conn(&cache);

    {
        txn_t txn(&cache_conn, write_durability_t::HARD, repli_timestamp_t::invalid, 1);
        buf_lock_t superblock(&txn, SUPERBLOCK_ID, alt_create_t::create);
        buf_write_t sb_write(&superblock);
        btree_slice_t::init_superblock(&superblock,
                                       std::vector<char>(), std::vector<char>());
    }

    order_source_t order_source;

    std::map<std::string, secondary_index_t> mirror;

    {
        scoped_ptr_t<txn_t> txn;
        scoped_ptr_t<real_superblock_t> superblock;
        get_btree_superblock_and_txn(&cache_conn, write_access_t::write, 1,
                                     repli_timestamp_t::invalid,
                                     write_durability_t::SOFT,
                                     &superblock, &txn);

        buf_lock_t sindex_block(superblock->expose_buf(),
                                superblock->get_sindex_block_id(),
                                access_t::write);

        initialize_secondary_indexes(&sindex_block);
    }

    for (int i = 0; i < 100; ++i) {
        std::string id = uuid_to_str(generate_uuid());

        secondary_index_t s;
        s.superblock = randint(1000);

        std::string opaque_blob = rand_string(1000);
        s.opaque_definition.assign(opaque_blob.begin(), opaque_blob.end());

        mirror[id] = s;

        scoped_ptr_t<txn_t> txn;
        scoped_ptr_t<real_superblock_t> superblock;
        get_btree_superblock_and_txn(&cache_conn, write_access_t::write, 1,
                                     repli_timestamp_t::invalid,
                                     write_durability_t::SOFT,
                                     &superblock, &txn);
        buf_lock_t sindex_block(superblock->expose_buf(),
                                superblock->get_sindex_block_id(),
                                access_t::write);

        set_secondary_index(&sindex_block, id, s);
    }

    {
        scoped_ptr_t<txn_t> txn;
        scoped_ptr_t<real_superblock_t> superblock;
        get_btree_superblock_and_txn(&cache_conn, write_access_t::write, 1,
                                     repli_timestamp_t::invalid,
                                     write_durability_t::SOFT,
                                     &superblock, &txn);
        buf_lock_t sindex_block(superblock->expose_buf(),
                                superblock->get_sindex_block_id(),
                                access_t::write);

        std::map<std::string, secondary_index_t> sindexes;
        get_secondary_indexes(&sindex_block, &sindexes);

        ASSERT_TRUE(sindexes == mirror);
    }
}

TEST(BTreeSindex, LowLevelOps) {
    run_in_thread_pool(&run_sindex_low_level_operations_test);
}

void run_sindex_btree_store_api_test() {
    temp_file_t temp_file;

    io_backender_t io_backender(file_direct_io_mode_t::buffered_desired);

    filepath_file_opener_t file_opener(temp_file.name(), &io_backender);
    standard_serializer_t::create(
        &file_opener,
        standard_serializer_t::static_config_t());

    standard_serializer_t serializer(
        standard_serializer_t::dynamic_config_t(),
        &file_opener,
        &get_global_perfmon_collection());

    rdb_protocol_t::store_t store(
            &serializer,
            "unit_test_store",
            GIGABYTE,
            true,
            &get_global_perfmon_collection(),
            NULL,
            &io_backender,
            base_path_t("."));

    cond_t dummy_interruptor;

    std::set<std::string> created_sindexs;

    for (int i = 0; i < 50; ++i) {
        std::string id = uuid_to_str(generate_uuid());
        created_sindexs.insert(id);
        {
            write_token_pair_t token_pair;
            store.new_write_token_pair(&token_pair);

            scoped_ptr_t<txn_t> txn;
            scoped_ptr_t<real_superblock_t> super_block;

            store.acquire_superblock_for_write(repli_timestamp_t::invalid,
                    1, write_durability_t::SOFT, &token_pair,
                    &txn, &super_block, &dummy_interruptor);

            buf_lock_t sindex_block
                = store.acquire_sindex_block_for_write(super_block->expose_buf(),
                                                       super_block->get_sindex_block_id());

            UNUSED bool b = store.add_sindex(id, std::vector<char>(), &sindex_block);
        }

        {
            write_token_pair_t token_pair;
            store.new_write_token_pair(&token_pair);

            scoped_ptr_t<txn_t> txn;
            scoped_ptr_t<real_superblock_t> super_block;

            store.acquire_superblock_for_write(repli_timestamp_t::invalid,
                                               1, write_durability_t::SOFT, &token_pair,
                                               &txn, &super_block, &dummy_interruptor);

            buf_lock_t sindex_block
                = store.acquire_sindex_block_for_write(
                    super_block->expose_buf(),
                    super_block->get_sindex_block_id());

            store.mark_index_up_to_date(id, &sindex_block);
        }

        {
            //Insert a piece of data in to the btree.
            write_token_pair_t token_pair;
            store.new_write_token_pair(&token_pair);

            scoped_ptr_t<txn_t> txn;
            scoped_ptr_t<real_superblock_t> super_block;

            store.acquire_superblock_for_write(
                    repli_timestamp_t::invalid, 1, write_durability_t::SOFT,
                    &token_pair, &txn, &super_block,
                    &dummy_interruptor);

            scoped_ptr_t<real_superblock_t> sindex_super_block;

            bool sindex_exists = store.acquire_sindex_superblock_for_write(
                    id,
                    super_block.get(),
                    &sindex_super_block);
            ASSERT_TRUE(sindex_exists);

            counted_t<const ql::datum_t> data = make_counted<ql::datum_t>(1.0);

            rdb_protocol_t::point_write_response_t response;
            rdb_modification_info_t mod_info;

            store_key_t key("foo");
            rdb_set(key, data, true, store.get_sindex_slice(id),
                    repli_timestamp_t::invalid,
                    sindex_super_block.get(), &response,
                    &mod_info, static_cast<profile::trace_t *>(NULL));
        }

        {
            //Read that data
            read_token_pair_t token_pair;
            store.new_read_token_pair(&token_pair);

            scoped_ptr_t<txn_t> txn;
            scoped_ptr_t<real_superblock_t> main_sb;
            scoped_ptr_t<real_superblock_t> sindex_super_block;

            store.acquire_superblock_for_read(
                    &token_pair.main_read_token, &txn, &main_sb,
                    &dummy_interruptor, true);

            store_key_t key("foo");

            bool sindex_exists = store.acquire_sindex_superblock_for_read(
                    id, main_sb.get(), &sindex_super_block,
                    static_cast<std::vector<char>*>(NULL));
            ASSERT_TRUE(sindex_exists);

            point_read_response_t response;

            rdb_get(key, store.get_sindex_slice(id),
                    sindex_super_block.get(), &response, NULL);

            ASSERT_EQ(ql::datum_t(1.0), *response.data);
        }
    }

    for (auto it  = created_sindexs.begin(); it != created_sindexs.end(); ++it) {
        /* Drop the sindex */
        write_token_pair_t token_pair;
        store.new_write_token_pair(&token_pair);

        scoped_ptr_t<txn_t> txn;
        scoped_ptr_t<real_superblock_t> super_block;

        store.acquire_superblock_for_write(repli_timestamp_t::invalid,
                                           1, write_durability_t::SOFT, &token_pair,
                                           &txn, &super_block, &dummy_interruptor);

        value_sizer_t<rdb_value_t> sizer(store.cache->get_block_size());

        rdb_value_deleter_t deleter;

        buf_lock_t sindex_block
            = store.acquire_sindex_block_for_write(super_block->expose_buf(),
                                                   super_block->get_sindex_block_id());

        // KSI: Probably the only reason drop_sindex takes a deleter is for weird
        // unit tests like this one.
        store.drop_sindex(
                *it, &sindex_block, &sizer, &deleter, &dummy_interruptor);
    }
}

TEST(BTreeSindex, BtreeStoreAPI) {
    run_in_thread_pool(&run_sindex_btree_store_api_test);
}

} // namespace unittest
