#include <mram.h>
#include <mutex.h>
#include <alloc.h>
#include <string.h>
#include <defs.h>

#include "mutex_def.h"
#include "debug.h"
#include "request_pool.h"
#include "dout.h"

#include "common.h"

/**
 * @brief Common structure to consume requests.
 *
 * Requests belong to a FIFO, from which each tasklet picks the reads. The FIFO is protected by a critical
 * section.
 *
 * @var mutex       Critical section that protects the pool.
 * @var nb_reads    The number of reads in the request pool.
 * @var rdidx       Index of the first unread read in the request pool.
 * @var cur_read    Address of the first read to be processed in MRAM.
 * @var cache       An internal cache to fetch requests from MRAM.
 * @var cache_size  The cache size
 */
typedef struct {
        mutex_t mutex;
        unsigned int nb_reads;
        unsigned int rdidx;
        mram_addr_t cur_read;
        unsigned int cache_size;
        uint8_t *cache;
        unsigned int stats_load;
} request_pool_t;

/**
 * @brief Common request pool, shared by every tasklet.
 */
static request_pool_t request_pool;

void request_pool_init(mram_info_t *mram_info)
{
        __attribute__((aligned(8))) request_info_t io_data;

        request_pool.mutex = mutex_get(MUTEX_REQUEST_POOL);

        REQUEST_INFO_READ(DPU_REQUEST_INFO_ADDR(mram_info), &io_data);
        request_pool.nb_reads = io_data.nb_reads;
        request_pool.rdidx = 0;
        request_pool.cur_read = (mram_addr_t) DPU_REQUEST_ADDR(mram_info);
        request_pool.cache_size = DPU_REQUEST_SIZE(mram_info->nbr_len);
        request_pool.cache = (uint8_t *) mem_alloc_dma(request_pool.cache_size);
        request_pool.stats_load = 0;
        DEBUG_REQUESTS_PRINT_POOL(request_pool);
}

bool request_pool_next(dpu_request_t *request, uint8_t *nbr, dpu_tasklet_stats_t *stats, mram_info_t *mram_info)
{
        mutex_lock(request_pool.mutex);
        if (request_pool.rdidx == request_pool.nb_reads) {
                mutex_unlock(request_pool.mutex);
                return false;
        }

        /* Fetch next request into cache */
        ASSERT_DMA_ADDR(request_pool.cur_read, request_pool.cache, request_pool.cache_size);
        ASSERT_DMA_LEN(request_pool.cache_size);
        mram_readX(request_pool.cur_read, (void *) request_pool.cache, request_pool.cache_size);
        stats->mram_load += request_pool.cache_size;
        stats->mram_data_load += request_pool.cache_size;
        request_pool.stats_load += request_pool.cache_size;

        memcpy(request, request_pool.cache, sizeof(dpu_request_t));
        memcpy(nbr, request_pool.cache + sizeof(dpu_request_t), mram_info->nbr_len);

        /* Point to next request */
        request_pool.rdidx++;
        request_pool.cur_read += request_pool.cache_size;
        mutex_unlock(request_pool.mutex);

        return true;
}

unsigned int request_pool_get_stats_load()
{
        return request_pool.stats_load;
}