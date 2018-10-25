/*
 * Copyright (c) 2014-2018 - uPmem
 */

#ifndef INTEGRATION_DEBUG_H
#define INTEGRATION_DEBUG_H

// Define DEBUG to get plain-text traces
#define DEBUG

// To print out every single request (pretty slow)
//#define DEBUG_REQUESTS
// To print the preliminary summary of MRAM mapping
//#define DEBUG_MRAM_INFO
// To print the intermediate computation results
//#define DEBUG_RESULTS
// To print the different stages of processing
//#define DEBUG_PROCESS
// To print ODPD results
//#define DEBUG_ODPD
// To print the different calls and occurrences in the main loop
//#define DEBUG_STATS
// To print the load/store MRAM performed by each tasklet
//#define DEBUG_STATS_MRAM
// Define NR_RUNNING_TASKLETS to define the effective number of operating tasklets (1 to 16)
#define NR_RUNNING_TASKLETS 16
// Define ASSERT_DMA_ADDR_ALIGNMENT to verify that the base addresses of a DMA transfer are
// properly aligned on 64 bits
//#define ASSERT_DMA_ADDR_ALIGNMENT

// To generate reference patterns for internal developments
//#define TRACE_PATTERNS_AND_SCORES

#include <ktrace.h>

#ifdef DEBUG
#define printf ktrace
#else
#define printf(...)
#endif

#ifndef NR_RUNNING_TASKLETS
#define NR_RUNNING_TASKLETS 16
#endif

#ifdef ASSERT_DMA_ADDR_ALIGNMENT

#include <mram.h>
#include <defs.h>

#ifndef MRAM_SIZE
#define MRAM_SIZE (64 << 20)
#endif // MRAM_SIZE
#ifndef WRAM_SIZE
#define WRAM_SIZE (128 << 10)
#endif // WRAM_SIZE

static inline void assert_dma_addr(mram_addr_t mram_addr, void *wram_addr, unsigned int len) {
    if ((mram_addr + len) > MRAM_SIZE) {
        printf("MRAM buffer [%x,%x[ does not fit in MRAM!\n", mram_addr, mram_addr + len);
        halt();
    }

    if (((unsigned int) wram_addr + len) > WRAM_SIZE) {
        printf("WRAM buffer [%x,%x[ does not fit in WRAM!\n", wram_addr, wram_addr + len);
        halt();
    }

    if ((mram_addr & 7) != 0) {
        printf("MRAM address %x is not aligned on longs!\n", mram_addr);
        halt();
    }
    if ((((unsigned int) wram_addr) & 7) != 0) {
        printf("WRAM address %x is not aligned on longs!\n", wram_addr);
        halt();
    }
}

static inline void assert_dma_len(unsigned int len) {
    if ((len & 7) != 0) {
        printf("DMA transfer len %u is not a multiple of longs!\n", len);
        halt();
    }
}

#else
#define assert_dma_addr(x, y, l)
#define assert_dma_len(x)
#endif // ASSERT_DMA_ALIGNMENT

#ifdef DEBUG_STATS
typedef struct stats {
    // Number of reads processed by this
    unsigned int nr_reads;
    // Number of references processed by this
    unsigned int nr_refs;
    // Number of calls to ODPD
    unsigned int nr_odpd_calls;
} *stats_t;

static inline void stat_inc_nr_reads(stats_t stats) {
    stats->nr_reads++;
}

static inline void stat_inc_nr_refs(stats_t stats) {
    stats->nr_refs++;
}

static inline void stat_inc_nr_odpd_calls(stats_t stats) {
    stats->nr_odpd_calls++;
}

static inline void stat_print(stats_t stats) {
    printf("STATS nr_reads=%u nr_refs=%u nr_odpd_calls=%u\n",
           stats->nr_reads,
           stats->nr_refs,
           stats->nr_odpd_calls);
}

static inline void stat_clear(stats_t stats) {
    stats->nr_reads = 0;
    stats->nr_refs = 0;
    stats->nr_odpd_calls = 0;
}

#else
#define stat_inc_nr_reads(x)
#define stat_inc_nr_refs(x)
#define stat_inc_nr_odpd_calls(x)
#define stat_print(x)
#define stat_clear(x)
#endif // DEBUG_STATS

#ifdef DEBUG_REQUESTS

#include <request.h>

/**
 * @brief Converts a byte stream to a neighbor
 * @param bytes   the byte stream
 * @param symbols filled with the corresponding sequence of symbols
 * @param nbr_len the byte stream length
 */
static void decode_nbr(const uint8_t *bytes, uint8_t *symbols, unsigned int nbr_len) {
    int i;
    for (i = 0; i < nr_bytes_to_syms(nbr_len); i += 4) {
        uint8_t this_byte = bytes[i >> 2];
        symbols[i] = (uint8_t) (this_byte & 3);
        symbols[i + 1] = (uint8_t) ((this_byte >> 2) & 3);
        symbols[i + 2] = (uint8_t) ((this_byte >> 4) & 3);
        symbols[i + 3] = (uint8_t) ((this_byte >> 6) & 3);
    }
}

/**
 * @brief Debug function to print a sequence of amino acids
 * @param symbols    the list of symbols
 * @param nr_symbols how many symbols
 */
static void print_symbols(const uint8_t *symbols, unsigned int nr_symbols) {
    int i;
    char as_char;
    for (i = 0; i < nr_symbols; i++) {
        switch (symbols[i]) {
            case 0:
                as_char = 'A';
                break;

            case 1:
                as_char = 'C';
                break;

            case 2 :
                as_char = 'T';
                break;

            default:
                as_char = 'G';
        }
        printf("%c", as_char);
    }
    printf("\n");
}

/**
 * @brief Prints a request
 * @param request  the request
 * @param nbr_len  neighbor length, in bytes
 */
static inline void debug_request(request_t *request, unsigned int nbr_len, uint8_t *symbols) {
    printf("offset=%u count=%u num=%u\n\t", request->offset, request->count, request->num);
    print_symbols(symbols, nr_bytes_to_syms(nbr_len));
}

static inline void debug_reference(unsigned int nbr_len, uint8_t *symbols) {
    printf("ref.\t");
    print_symbols(symbols, nr_bytes_to_syms(nbr_len));
}
#else
#define debug_request(request, nbr_len, symbols) do {} while(0)
#define debug_reference(nbr_len, symbols) do {} while(0)
#endif /* DEBUG_REQUESTS */

#ifdef TRACE_PATTERNS_AND_SCORES

#include <mutex.h>

static inline void trace_pattern_profile(unsigned int nbr_len_in_bytes) {
    printf("NBRLEN:%u\n", nbr_len_in_bytes);
}

static inline void trace_pattern(const char *name, unsigned int nr_bytes, uint8_t *req) {
    // Cheating a little bit with mutexes...
    unsigned int each_byte;
    printf("%s:", name);
    for (each_byte = 0; each_byte < nr_bytes; each_byte++) printf(" %02x", req[each_byte]);
    printf("\n");
}

static inline void trace_score(const char *name, int value) {
    printf("score %s %u\n", name, value);
}

static inline void trace_pattern_and_scores(unsigned int nr_bytes, uint8_t *req, uint8_t *ref,
                                            int mini, int score_nodp, bool has_odpd, int score_odpd,
                                            mutex_t lockit) {
    mutex_lock(lockit);
    trace_pattern("REQ", nr_bytes, req);
    trace_pattern("REF", nr_bytes, ref);
    printf("mini %u\n", mini);
    trace_score("nodp", score_nodp);
    if (has_odpd) trace_score("odpd", score_odpd);
    mutex_unlock(lockit);
}

#else
#define trace_pattern_profile(n) do {} while(0)
#define trace_pattern_and_scores(n, req, ref, mini, snodp, hasodpd, sodpd, lockit) do {} while(0)
#endif // TRACE_PATTERNS_AND_SCORES

#ifdef DEBUG_STATS_MRAM

#include <mutex.h>
#include <defs.h>

static inline void print_mram_stats(unsigned int stats_load, unsigned int stats_store, mutex_t lockit) {
    mutex_lock(lockit);
    printf("mem stats: reads=%d writes=%d\n", stats_load, stats_store);
    mutex_unlock(lockit);
}

#else
#define print_mram_stats(l, s, m) do {} while(0)
#endif // DEBUG_STATS_MRAM

#endif //INTEGRATION_DEBUG_H
