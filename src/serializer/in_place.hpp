
#ifndef __IN_PLACE_SERIALIZER_HPP__
#define __IN_PLACE_SERIALIZER_HPP__

#include <sys/types.h>
#include <sys/stat.h>
#include <unistd.h>
#include <fcntl.h>
#include "arch/resource.hpp"
#include "btree/admin.hpp"

// TODO: what about multiple modifications to the tree that need to be
// performed atomically?

// TODO: a robust implementation requires a replay log.

// TODO: how to maintain the id of the root block?

/* This is a serializer that writes blocks in place. It should be
 * efficient for rotational drives and flash drives with a very good
 * FTL. It's also a good sanity check that the rest of the system
 * isn't tightly coupled with a log-structured serializer. */
template<class config_t>
struct in_place_serializer_t {

public:
    typedef typename config_t::btree_admin_t btree_admin_t;

    in_place_serializer_t(char *db_path, size_t _block_size)
        : block_size(_block_size), dbfd(-1), dbsize(-1) {
        // Open the DB file
        dbfd = open(db_path,
                    O_RDWR | O_CREAT | O_DIRECT | O_LARGEFILE | O_NOATIME,
                    S_IRUSR | S_IWUSR | S_IRGRP | S_IWGRP);
        check("Could not open database file", dbfd == -1);
        
        // Determine last block id
        dbsize = lseek64(dbfd, 0, SEEK_END);
        check("Could not determine database file size", dbsize == -1);
        off64_t res = lseek64(dbfd, 0, SEEK_SET);
        check("Could not reset database file position", res == -1);
        
        // Leave space for the metablock if necessary
        if(dbsize == 0) {
            // This crosses a boundary that ideally shouldn't be crossed, because the rest of the
            // buffer cache code doesn't know that the buffer cache is being used to store a btree.
            // This is the only part of the buffer cache code that references the btree code.
            btree_admin_t::create_db(dbfd);
            dbsize = block_size;
        }
    }
    ~in_place_serializer_t() {
        ::close(dbfd);
    }
    
    off64_t id_to_offset(block_id_t id) {
        return id * block_size;
    }
    
    /* Fires off an async request to read the block identified by
     * block_id into buf, associating callback with the request. */
    void do_read(event_queue_t *queue, block_id_t block_id, void *buf,
                 iocallback_t *callback) {
        queue->iosys.schedule_aio_read(dbfd, id_to_offset(block_id), block_size, buf, queue, callback);
    }
    
    /* Fires off async requests to write the blocks identified by
     * block_ids into bufs, associating callbacks with the request.
     * The IO request must be asynchronous, and not just for
     * performance reasons -- if the callback is called before
     * do_write() returns, then writeback_t::writeback() will be
     * confused. */
    struct write {
        block_id_t    block_id;
        void          *buf;
        iocallback_t  *callback;
    };

    void do_write(event_queue_t *queue, write *writes, int num_writes) {
        // TODO: watch how we're allocating
        io_calls_t::aio_write_t aio_writes[num_writes];
        int i;

        for (i = 0; i < num_writes; i++) {
            aio_writes[i].resource = dbfd;
            aio_writes[i].offset = id_to_offset(writes[i].block_id);
            aio_writes[i].length = block_size;
            aio_writes[i].buf = writes[i].buf;
            aio_writes[i].callback = writes[i].callback;
        }

        queue->iosys.schedule_aio_write(aio_writes, num_writes, queue);
    }

    static const block_id_t null_block_id = -1;

    /* Returns true iff block_id is NULL. */
    static bool is_block_id_null(block_id_t block_id) {
        return block_id == null_block_id;
    }

    /* Generates a unique block id. */
    block_id_t gen_block_id() {
        off64_t new_block_id = dbsize / block_size;
        dbsize += block_size;
        return new_block_id;
    }

    /* Consumer of the serializer can store bootstrapping information
     * in the superblock. For the in place serializer the superblock
     * is always at the beginning of the file. */
    block_id_t get_superblock_id() {
        return 0;
    }

    size_t block_size;

private:
    resource_t dbfd;
    off64_t dbsize;
};

#endif // __IN_PLACE_SERIALIZER_HPP__

