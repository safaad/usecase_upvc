/*
 * Copyright (c) 2014-2018 - uPmem
 */

#ifndef INTEGRATION_ODPD_H
#define INTEGRATION_ODPD_H

#include <stdint.h>

/**
 * @brief Compare two sets of symbols to produce a score.
 *
 * The module implements a version of Smith&Waterman algorithm. It first requires initializing
 * memory resources, before invoking the comparison function odpd.
 */

/**
 * @brief Maximum score allowed.
 */
#define MAX_SCORE        40

/**
 * @brief Sets up an operating environment for the comparator to run on several tasklets in parallel.
 *
 * @param nr_tasklets how many tasklets will work
 * @param nbr_sym_len size of a neighbor, in number of symbols
 */
void odpd_init(unsigned int nr_tasklets, unsigned int nbr_sym_len);

/**
 * @brief Compares two sequences of symbols to assign a score.
 *
 * Optimal Dynamic Programming Diagonal. The sequences are expressed a byte streams, i.e. 4
 * nucleotides per bytes.
 *
 * @param s1 the first vector
 * @param s2 the second vector
 * @param max_score any score above this threshold is good
 * @param nbr_sym_len the number of symbols in s1 and s2
 * @param tid sysname of the invoking tasklet
 * @return A score
 */
int odpd(const uint8_t *s1, const uint8_t *s2, int max_score, unsigned int nbr_sym_len, unsigned int tid);

// Assembler version
int odpdx(const uint8_t *s1, const uint8_t *s2, int max_score, unsigned int nbr_sym_len, unsigned int tid);
#endif //INTEGRATION_ODPD_H
