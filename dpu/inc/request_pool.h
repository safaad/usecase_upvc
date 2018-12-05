#ifndef __REQUEST_POOL_H__
#define __REQUEST_POOL_H__

#include <mram.h>

#include "common.h"
#include "dout.h"

/**
 * @brief Gets the next read from the request pool, if any.
 *
 * @param request_buffer  Output Request filled with the next request and corresponding neighbour.
 * @param stats           To update statistical reports.
 *
 * @return True if a new request was fetched, false if the FIFO is empty.
 */
bool request_pool_next(uint8_t *request_buffer, dpu_tasklet_stats_t *stats);

/**
 * @brief Initializes the request pool.
 */
void request_pool_init(mram_info_t *mram_info);

#endif /* __REQUEST_POOL_H__ */
