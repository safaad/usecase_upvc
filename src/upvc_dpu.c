#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>

#include "upvc_dpu.h"
#include "upvc.h"

/* DPU memory layout */
typedef struct {
        int8_t *neighbour_idx;      /* datas : table of the neighbours of the seed from the reference genome in the DPU */
        long *coordinate;           /* datas : [sequence id, offset in sequence] of each seed in the DPU                */
        int8_t *neighbour_read;     /* datas : table of neighbours                                                      */
        int *offset;                /* datas : address of the first neighbour                                           */
        int *count;                 /* datas : number of neighbour for each read                                        */
        int *num;                   /* datas : reads id                                                                 */

        int *out_score;             /* output : score                                                                   */
        int *out_num;               /* output : number of the read that matched                                         */
        long *out_coord;            /* output : coordinate on the genome where the read matched                         */
} MEM_DPU;

static MEM_DPU *MDPU;

/* Statistics */
static long stat_nb_read[NB_DPU]; /* Number of reads dispatched by DPU */
static long stat_nb_nbr[NB_DPU];  /* Number of distances computed by DPU */

static int min (int a, int b)
{
        return a < b ? a : b;
}

#define PQD_INIT_VAL (99)
static int ODPD_compute(int i,
                        int j,
                        int8_t *s1,
                        int8_t *s2,
                        int *Pppj,
                        int Pppjm,
                        int *Qppj,
                        int Qlpj,
                        int *Dppj,
                        int Dppjm,
                        int Dlpj,
                        int Dlpjm)
{
        int d = Dlpjm;
        int QP;
        *Pppj = min(Dppjm + COST_GAPO, Pppjm + COST_GAPE);
        *Qppj = min(Dlpj  + COST_GAPO, Qlpj  + COST_GAPE);
        QP = min(*Pppj, *Qppj);
        if ( ( (s1[(i - 1) / 4] >> (2 * ((i - 1) % 4))) & 3)
             !=
             ( (s2[(j - 1) / 4] >> (2 * ((j - 1) % 4))) & 3) ) {
                d += COST_SUB;
        }
        *Dppj = min(d, QP);
}

/*
 * Compute the alignment distance by dynamical programming on the diagonals of the matrix.
 * Stops when score is greater than max_score
 */
static int ODPD(int8_t *s1, int8_t *s2, int max_score, reads_info_t *reads_info)
{
        int size_neighbour = reads_info->size_neighbour_in_32bits_words;
        int matrix_size = size_neighbour+1;
        int D[2][matrix_size];
        int P[2][matrix_size];
        int Q[2][matrix_size];
        int diagonal = (NB_DIAG / 2) + 1;

        for (int j = 0; j <= diagonal; j++) {
                P[0][j] = PQD_INIT_VAL;
                Q[0][j] = PQD_INIT_VAL;
                D[0][j] = j * COST_SUB;
        }
        P[1][0]=PQD_INIT_VAL;
        Q[1][0]=PQD_INIT_VAL;

        for (int i = 1; i < diagonal; i++) {
                int min_score = PQD_INIT_VAL;
                int pp = i % 2;
                int lp = (i - 1) % 2;
                D[pp][0] = i * COST_SUB;
                for (int j = 1; j < i + diagonal; j++) {
                        ODPD_compute(i, j, s1, s2,
                             &P[pp][j], P[pp][j - 1],
                             &Q[pp][j], Q[lp][j],
                             &D[pp][j], D[pp][j - 1], D[lp][j], D[lp][j - 1]);
                        if (D[pp][j] < min_score) {
                                min_score = D[pp][j];
                        }
                }
                Q[pp][i + diagonal]=PQD_INIT_VAL;
                D[pp][i + diagonal]=PQD_INIT_VAL;
                if (min_score > max_score) {
                        return min_score;
                }
        }

        for (int i = diagonal; i < matrix_size - diagonal; i++) {
                int min_score = PQD_INIT_VAL;
                int pp = i % 2;
                int lp = (i - 1) % 2;
                P[pp][i - diagonal] = PQD_INIT_VAL;
                D[pp][i - diagonal] = PQD_INIT_VAL;
                for (int j = i + 1 - diagonal; j < i + diagonal; j++) {
                        ODPD_compute(i, j, s1, s2,
                             &P[pp][j], P[pp][j - 1],
                             &Q[pp][j], Q[lp][j],
                             &D[pp][j], D[pp][j - 1], D[lp][j], D[lp][j - 1]);
                        if (D[pp][j] < min_score) {
                                min_score = D[pp][j];
                        }
                }
                Q[pp][i + diagonal]=PQD_INIT_VAL;
                D[pp][i + diagonal]=PQD_INIT_VAL;
                if (min_score > max_score) {
                        return min_score;
                }
        }
        int min_score = PQD_INIT_VAL;
        for (int i = matrix_size - diagonal; i < matrix_size; i++) {
                int pp = i % 2;
                int lp = (i - 1) % 2;
                P[pp][i - diagonal] = PQD_INIT_VAL;
                D[pp][i - diagonal] = PQD_INIT_VAL;
                for (int j = i + 1 - diagonal; j < matrix_size; j++) {
                        ODPD_compute(i, j, s1, s2,
                             &P[pp][j], P[pp][j - 1],
                             &Q[pp][j], Q[lp][j],
                             &D[pp][j], D[pp][j - 1], D[lp][j], D[lp][j - 1]);
                }
                if (D[pp][matrix_size - 1] < min_score)
                        min_score = D[pp][matrix_size - 1];
        }
        int i=matrix_size - 1;
        int pp = i%2;
        for (int j = i + 1 - diagonal; j < matrix_size; j++) {
                if (D[pp][j] < min_score) {
                        min_score = D[pp][j];
                }
        }

        return min_score;
}

static int translation_table[256] = {0 , 10 , 10 , 10 , 10 , 20 , 20 , 20 , 10 , 20 , 20 , 20 , 10 , 20 , 20 , 20 ,
                                     10 , 20 , 20 , 20 , 20 , 30 , 30 , 30 , 20 , 30 , 30 , 30 , 20 , 30 , 30 , 30 ,
                                     10 , 20 , 20 , 20 , 20 , 30 , 30 , 30 , 20 , 30 , 30 , 30 , 20 , 30 , 30 , 30 ,
                                     10 , 20 , 20 , 20 , 20 , 30 , 30 , 30 , 20 , 30 , 30 , 30 , 20 , 30 , 30 , 30 ,
                                     10 , 20 , 20 , 20 , 20 , 30 , 30 , 30 , 20 , 30 , 30 , 30 , 20 , 30 , 30 , 30 ,
                                     20 , 30 , 30 , 30 , 30 , 40 , 40 , 40 , 30 , 40 , 40 , 40 , 30 , 40 , 40 , 40 ,
                                     20 , 30 , 30 , 30 , 30 , 40 , 40 , 40 , 30 , 40 , 40 , 40 , 30 , 40 , 40 , 40 ,
                                     20 , 30 , 30 , 30 , 30 , 40 , 40 , 40 , 30 , 40 , 40 , 40 , 30 , 40 , 40 , 40 ,
                                     10 , 20 , 20 , 20 , 20 , 30 , 30 , 30 , 20 , 30 , 30 , 30 , 20 , 30 , 30 , 30 ,
                                     20 , 30 , 30 , 30 , 30 , 40 , 40 , 40 , 30 , 40 , 40 , 40 , 30 , 40 , 40 , 40 ,
                                     20 , 30 , 30 , 30 , 30 , 40 , 40 , 40 , 30 , 40 , 40 , 40 , 30 , 40 , 40 , 40 ,
                                     20 , 30 , 30 , 30 , 30 , 40 , 40 , 40 , 30 , 40 , 40 , 40 , 30 , 40 , 40 , 40 ,
                                     10 , 20 , 20 , 20 , 20 , 30 , 30 , 30 , 20 , 30 , 30 , 30 , 20 , 30 , 30 , 30 ,
                                     20 , 30 , 30 , 30 , 30 , 40 , 40 , 40 , 30 , 40 , 40 , 40 , 30 , 40 , 40 , 40 ,
                                     20 , 30 , 30 , 30 , 30 , 40 , 40 , 40 , 30 , 40 , 40 , 40 , 30 , 40 , 40 , 40 ,
                                     20 , 30 , 30 , 30 , 30 , 40 , 40 , 40 , 30 , 40 , 40 , 40 , 30 , 40 , 40 , 40 };

/*
 * Optimized version of ODPD (if no INDELS)
 * If it detects INDELS, return -1. In this case we will need the run the full ODPD.
 */
static int noDP(int8_t *s1, int8_t *s2, int max_score, reads_info_t *reads_info)
{
        int score = 0;
        int size_neighbour = reads_info->size_neighbour_in_bytes;
        int delta_neighbour = reads_info->delta_neighbour_in_bytes;
        for (int i = 0; i < size_neighbour - delta_neighbour; i++) {
                int s_xor = ((int) (s1[i] ^ s2[i])) & 0xFF;
                int s_translated = translation_table[s_xor];
                if (s_translated > COST_SUB) {
                        int j = i + 1;
                        /* INDELS detection */
                        if (j < size_neighbour - delta_neighbour - 3) {
                                int s1_val = ((int *) (&s1[j]))[0];
                                int s2_val = ((int *) (&s2[j]))[0];
                                if ( ((s1_val ^ (s2_val >> 2)) & 0x3FFFFFFF) == 0) {
                                        return -1;
                                }
                                if ( ((s1_val ^ (s2_val >> 4)) & 0xFFFFFFF) == 0) {
                                        return -1;
                                }
                                if ( ((s1_val ^ (s2_val >> 6)) & 0x3FFFFFF) == 0) {
                                        return -1;
                                }
                                if ( ((s1_val ^ (s2_val >> 8)) & 0xFFFFFF) == 0) {
                                        return -1;
                                }
                                if ( ((s2_val ^ (s1_val >> 2)) & 0x3FFFFFFF) == 0) {
                                        return -1;
                                }
                                if ( ((s2_val ^ (s1_val >> 4)) & 0xFFFFFFF) == 0) {
                                        return -1;
                                }
                                if ( ((s2_val ^ (s1_val >> 6)) & 0x3FFFFFF) == 0) {
                                        return -1;
                                }
                                if ( ((s2_val ^ (s1_val >> 8)) & 0xFFFFFF) == 0) {
                                        return -1;
                                }
                        }
                }
                score += s_translated;
                if (score > max_score)
                        break;
        }
        return score;
}

static void print2NBR(int8_t *s1, int8_t *s2, reads_info_t *reads_info)
{
        int size_neighbour = reads_info->size_neighbour_in_bytes;
        for (int i = 0; i < size_neighbour; i++) {
                for (int j = 0; j < 4; j++) {
                        printf("%x", (s1[i] >> (2 * j)) & 3);
                }
        }
        printf("\n");
        for (int i = 0; i < size_neighbour; i++) {
                for (int j = 0; j < 4; j++) {
                        if ( ((s1[i] >> (2 * j)) & 3) != ((s2[i] >> (2 * j)) & 3) ) {
                                printf("|");
                        } else {
                                printf(" ");
                        }
                }
        }
        printf("\n");
        for (int i = 0; i < size_neighbour; i++) {
                for (int j = 0; j < 4; j++) {
                        printf("%x", (s2[i] >> (2 * j)) & 3);
                }
        }
        printf("\n\n");
}


#define MAX_SCORE        40

void *align_on_dpu(void *arg)
{
        int nb_map = 0;
        align_on_dpu_arg_t *dpu_arg = (align_on_dpu_arg_t *)arg;
        int numdpu = dpu_arg->numdpu;
        reads_info_t *reads_info = &(dpu_arg->reads_info);
        int size_neighbour = reads_info->size_neighbour_in_bytes;
        MEM_DPU M = MDPU[numdpu];
        int current_read = 0;

        stat_nb_read[numdpu]=0;
        stat_nb_nbr[numdpu]=0;

        M.out_num[nb_map] = -1;
        while (M.num[current_read] != -1) {
                int offset = M.offset[current_read]; /* address of the first neighbour */
                int min = MAX_SCORE;
                int nb_map_start = nb_map;
                for (int nb_neighbour = 0; nb_neighbour < M.count[current_read]; nb_neighbour++) {
                        int score_noDP =noDP(&M.neighbour_read[current_read*size_neighbour],
                                             &M.neighbour_idx[(offset+nb_neighbour)*size_neighbour],
                                             min,
                                             reads_info);
                        int score = score_noDP;
                        if (score_noDP == -1) {
                                int score_ODPD = ODPD(&M.neighbour_read[current_read*size_neighbour],
                                                      &M.neighbour_idx[(offset+nb_neighbour)*size_neighbour],
                                                      min,
                                                      reads_info);
                                score = score_ODPD;
                        }
                        if (score <= min) {
                                if (score <  min) {
                                        min = score;
                                        nb_map = nb_map_start;
                                }
                                if (nb_map < MAX_ALIGN-1) {
                                        M.out_num[nb_map] = M.num[current_read];
                                        M.out_coord[nb_map] = M.coordinate[offset+nb_neighbour];
                                        M.out_score[nb_map] = score;
                                        nb_map++;
                                        M.out_num[nb_map] = -1;
                                }
                        }
                }
                current_read++;
        }
        return NULL;
}


void malloc_dpu(reads_info_t *reads_info)
{
        MDPU = (MEM_DPU *) malloc(sizeof(MEM_DPU) * NB_DPU);
        for (int num_dpu = 0; num_dpu < NB_DPU; num_dpu++) {
                MDPU[num_dpu].neighbour_read =
                        (int8_t *) malloc(sizeof(int8_t) * reads_info->size_neighbour_in_bytes * MAX_NB_DPU_READ);
                MDPU[num_dpu].count          = (int *)    malloc(sizeof(int) * MAX_NB_DPU_READ);
                MDPU[num_dpu].offset         = (int *)    malloc(sizeof(int) * MAX_NB_DPU_READ);
                MDPU[num_dpu].num            = (int *)    malloc(sizeof(int) * MAX_NB_DPU_READ);
                MDPU[num_dpu].out_num        = (int *)    malloc(sizeof(int) * MAX_ALIGN);
                MDPU[num_dpu].out_coord      = (long *)   malloc(sizeof(long) * MAX_ALIGN);
                MDPU[num_dpu].out_score      = (int  *)   malloc(sizeof(long) * MAX_ALIGN);
        }
}

void malloc_neighbour_idx (int num_dpu, int nb_index, reads_info_t *reads_info)
{
        MDPU[num_dpu].neighbour_idx = (int8_t *) malloc(sizeof(int8_t) * nb_index * reads_info->size_neighbour_in_bytes);
        MDPU[num_dpu].coordinate = (long *) malloc(sizeof(long) * nb_index);
}

void free_dpu()
{
        for (int num_dpu = 0; num_dpu<NB_DPU; num_dpu++) {
                free (MDPU[num_dpu].neighbour_idx);
                free (MDPU[num_dpu].neighbour_read);
                free (MDPU[num_dpu].coordinate);
                free (MDPU[num_dpu].count);
                free (MDPU[num_dpu].offset);
                free (MDPU[num_dpu].num);
                free (MDPU[num_dpu].out_num);
                free (MDPU[num_dpu].out_score);
                free (MDPU[num_dpu].out_coord);
        }
        free(MDPU);
}

void write_neighbour_read (int num_dpu, int read_idx, int8_t *values, reads_info_t *reads_info)
{
        int size_neighbour = reads_info->size_neighbour_in_bytes;
        for (int i = 0; i < size_neighbour; i++) {
                MDPU[num_dpu].neighbour_read[read_idx * size_neighbour + i] = values[i];
        }
}

void write_neighbour_idx (int num_dpu, int index_idx, int8_t *values, reads_info_t *reads_info)
{
        int i;
        int size_neighbour = reads_info->size_neighbour_in_bytes;
        for (int i = 0; i < size_neighbour; i++) {
                MDPU[num_dpu].neighbour_idx[index_idx * size_neighbour + i] = values[i];
        }
}

void write_coordinate  (int num_dpu, int read_idx, long value) { MDPU[num_dpu].coordinate[read_idx] = value; }
void write_count       (int num_dpu, int read_idx, int value)  { MDPU[num_dpu].count[read_idx]      = value; }
void write_offset      (int num_dpu, int read_idx, int value)  { MDPU[num_dpu].offset[read_idx]     = value; }
void write_num         (int num_dpu, int read_idx, int value)  { MDPU[num_dpu].num[read_idx]        = value; }

int read_out_num       (int num_dpu, int align_idx) { return MDPU[num_dpu].out_num[align_idx];   }
int read_out_score     (int num_dpu, int align_idx) { return MDPU[num_dpu].out_score[align_idx]; }
long read_out_coord    (int num_dpu, int align_idx) { return MDPU[num_dpu].out_coord[align_idx]; }
