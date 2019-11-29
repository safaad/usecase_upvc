/**
 * Copyright 2016-2019 - Dominique Lavenier & UPMEM
 */

#include <stddef.h>
#include <stdlib.h>
#include <string.h>

#include "dispatch.h"
#include "dpu_log.h"
#include "dpus_mgmt.h"
#include "mram_dpu.h"
#include "parse_args.h"
#include "upvc.h"

#include "upvc_dpu.h"

#define XSTR(s) STR(s)
#define STR(s) #s

static char *profile;

void setup_dpus_for_target_type(target_type_t target_type)
{
    switch (target_type) {
    case target_type_hw:
        profile = "backend=hw,cycleAccurate=true";
        break;
    default:
        profile = "backend=simulator";
        break;
    }
}

devices_t *dpu_try_alloc_for(unsigned int nb_dpus_per_run, const char *opt_program)
{
    dpu_api_status_t status;
    uint32_t nb_dpus;
    devices_t *devices = (devices_t *)malloc(sizeof(devices_t));
    assert(devices != NULL);

    status = dpu_alloc_dpus(profile, nb_dpus_per_run, &devices->ranks, &devices->nb_ranks_per_run, &nb_dpus);
    assert(status == DPU_API_SUCCESS && "dpu_alloc_dpus failed");

    if (nb_dpus_per_run == DPU_ALLOCATE_ALL)
        set_nb_dpus_per_run(nb_dpus_per_run = nb_dpus);
    else if (nb_dpus < nb_dpus_per_run)
        ERROR_EXIT(5, "Only %u dpus available for %u requested\n", nb_dpus, nb_dpus_per_run);

    status = dpu_get_nr_of_dpus_in(devices->ranks[0], &devices->nb_dpus_per_rank);
    assert(status == DPU_API_SUCCESS && "dpu_get_nr_of_dpus_in failed");

    for (unsigned int each_rank = 0; each_rank < devices->nb_ranks_per_run; each_rank++) {
        status = dpu_load_all(devices->ranks[each_rank], opt_program);
        assert(status == DPU_API_SUCCESS && "dpu_load_all failed");
    }

    struct dpu_t *one_dpu;
    DPU_FOREACH (devices->ranks[0], one_dpu) {
        status = dpu_get_mram_symbol(one_dpu, DPU_MRAM_HEAP_POINTER_NAME, &devices->mram_available_addr, NULL);
        assert(status == DPU_API_SUCCESS && "dpu_get_mram_symbol failed");
        status = dpu_get_mram_symbol(one_dpu, XSTR(DPU_MRAM_INFO_VAR), &devices->mram_info_addr, NULL);
        assert(status == DPU_API_SUCCESS && "dpu_get_mram_symbol failed");
        status = dpu_get_mram_symbol(one_dpu, XSTR(DPU_REQUEST_INFO_VAR), &devices->mram_request_info_addr, NULL);
        assert(status == DPU_API_SUCCESS && "dpu_get_mram_symbol failed");
        status = dpu_get_mram_symbol(one_dpu, XSTR(DPU_REQUEST_VAR), &devices->mram_requests_addr, &devices->mram_requests_size);
        assert(status == DPU_API_SUCCESS && "dpu_get_mram_symbol failed");
        status = dpu_get_mram_symbol(one_dpu, XSTR(DPU_COMPUTE_TIME_VAR), &devices->mram_compute_time_addr, NULL);
        assert(status == DPU_API_SUCCESS && "dpu_get_mram_symbol failed");
        status = dpu_get_mram_symbol(one_dpu, XSTR(DPU_TASKLET_STATS_VAR), &devices->mram_tasklet_stats_addr, NULL);
        assert(status == DPU_API_SUCCESS && "dpu_get_mram_symbol failed");
        status = dpu_get_mram_symbol(one_dpu, XSTR(DPU_RESULT_VAR), &devices->mram_result_addr, &devices->mram_result_size);
        assert(status == DPU_API_SUCCESS && "dpu_get_mram_symbol failed");
        break;
    }
    devices->mram_available_size = MRAM_SIZE - (devices->mram_available_addr & (MRAM_SIZE - 1));

    pthread_mutex_init(&devices->log_mutex, NULL);
    devices->log_file = fopen("upvc_log.txt", "w");

    return devices;
}

void dpu_try_write_mram(unsigned int rank_id, devices_t *devices, uint8_t **mram, size_t *mram_size, int delta_neighbour)
{
    dpu_api_status_t status;
    struct dpu_rank_t *rank = devices->ranks[rank_id];
    struct dpu_transfer_mram *matrix, *matrix_delta;
    status = dpu_transfer_matrix_allocate(rank, &matrix);
    assert(status == DPU_API_SUCCESS && "dpu_transfer_matrix_allocate failed");
    status = dpu_transfer_matrix_allocate(rank, &matrix_delta);
    assert(status == DPU_API_SUCCESS && "dpu_transfer_matrix_allocate failed");

    unsigned int each_dpu = 0;
    struct dpu_t *dpu;
    DPU_FOREACH(rank, dpu) {
        dpu_transfer_matrix_add_dpu(
            dpu, matrix, mram[each_dpu], devices->mram_available_size, devices->mram_available_addr, 0);
        dpu_transfer_matrix_add_dpu(dpu, matrix_delta, &delta_neighbour, sizeof(delta_info_t), devices->mram_info_addr, 0);
        assert(mram_size[each_dpu] < devices->mram_available_size && "mram is to big to fit in DPU");
        each_dpu++;
    }

    status = dpu_copy_to_dpus(rank, matrix);
    assert(status == DPU_API_SUCCESS && "dpu_copy_to_dpus failed");
    status = dpu_copy_to_dpus(rank, matrix_delta);
    assert(status == DPU_API_SUCCESS && "dpu_copy_to_dpus failed");
    dpu_transfer_matrix_free(rank, matrix);
    dpu_transfer_matrix_free(rank, matrix_delta);
}

void dpu_try_free(devices_t *devices)
{
    for (unsigned int each_rank = 0; each_rank < devices->nb_ranks_per_run; each_rank++) {
        dpu_free(devices->ranks[each_rank]);
    }
    pthread_mutex_destroy(&devices->log_mutex);
    fclose(devices->log_file);
    free(devices->ranks);
    free(devices);
}

void dpu_try_run(unsigned int rank_id, devices_t *devices)
{
    dpu_api_status_t status = dpu_boot_all(devices->ranks[rank_id], ASYNCHRONOUS);
    assert(status == DPU_API_SUCCESS && "dpu_boot_all failed");
}

bool dpu_try_check_status(unsigned int rank_id, devices_t *devices)
{
    dpu_api_status_t status;
    unsigned int nb_dpus_per_rank;
    struct dpu_rank_t *rank = devices->ranks[rank_id];
    uint32_t nb_dpus_running = 0;

    status = dpu_get_nr_of_dpus_in(rank, &nb_dpus_per_rank);
    assert (status == DPU_API_SUCCESS && "dpu_get_nr_of_dpus_in failed");
    dpu_run_status_t run_status[nb_dpus_per_rank];

    status = dpu_get_all_status(devices->ranks[rank_id], run_status, &nb_dpus_running);
    assert(status == DPU_API_SUCCESS && "dpu_get_all_status failed");

    struct dpu_t *dpu;
    unsigned int each_dpu = 0;
    DPU_FOREACH(rank, dpu) {
        switch (run_status[each_dpu]) {
        case DPU_STATUS_IDLE:
        case DPU_STATUS_RUNNING:
            continue;
        case DPU_STATUS_ERROR:
            dpulog_read_for_dpu(dpu, stdout);
            ERROR_EXIT(10, "*** DPU %u.%u reported an error - aborting", rank_id, each_dpu);
        default:
            dpulog_read_for_dpu(dpu, stdout);
            ERROR_EXIT(11, "*** could not get DPU %u.%u status %u - aborting", rank_id, each_dpu, run_status[each_dpu]);
        }
        each_dpu++;
    }
    return (nb_dpus_running == 0);
}

void dpu_try_write_dispatch_into_mram(
    unsigned int rank_id, unsigned int dpu_offset, devices_t *devices, dispatch_request_t *dispatch)
{
    dpu_api_status_t status;
    struct dpu_rank_t *rank = devices->ranks[rank_id];
    struct dpu_transfer_mram *matrix_header, *matrix_reads;

    unsigned int nb_dpus_per_rank;
    status = dpu_get_nr_of_dpus_in(rank, &nb_dpus_per_rank);
    assert(status == DPU_API_SUCCESS && "dpu_get_nr_of_dpus_in failed");
    request_info_t io_header[nb_dpus_per_rank];

    status = dpu_transfer_matrix_allocate(rank, &matrix_header);
    assert(status == DPU_API_SUCCESS && "dpu_transfer_matrix_allocate failed");
    status = dpu_transfer_matrix_allocate(rank, &matrix_reads);
    assert(status == DPU_API_SUCCESS && "dpu_transfer_matrix_allocate failed");

    struct dpu_t *dpu;
    unsigned int each_dpu = 0;
    DPU_FOREACH(rank, dpu) {
        unsigned int nb_reads = dispatch[each_dpu + dpu_offset].nb_reads;
        io_header[each_dpu].nb_reads = nb_reads;

        dpu_transfer_matrix_add_dpu(
            dpu, matrix_header, &io_header[each_dpu], sizeof(request_info_t), devices->mram_request_info_addr, 0);
        dpu_transfer_matrix_add_dpu(dpu, matrix_reads, dispatch[each_dpu + dpu_offset].reads_area, devices->mram_requests_size,
            devices->mram_requests_addr, 0);
        each_dpu++;
    }

    status = dpu_copy_to_dpus(rank, matrix_header);
    assert(status == DPU_API_SUCCESS && "dpu_copy_to_dpus failed");
    status = dpu_copy_to_dpus(rank, matrix_reads);
    assert(status == DPU_API_SUCCESS && "dpu_copy_to_dpus failed");

    dpu_transfer_matrix_free(rank, matrix_header);
    dpu_transfer_matrix_free(rank, matrix_reads);
}

#define CLOCK_PER_SECONDS (600000000.0)
static void dpu_try_log(unsigned int rank_id, unsigned int dpu_offset, devices_t *devices, struct dpu_transfer_mram *matrix)
{
    dpu_api_status_t status;
    struct dpu_rank_t *rank = devices->ranks[rank_id];
    unsigned int nb_dpus_per_rank;
    status = dpu_get_nr_of_dpus_in(rank, &nb_dpus_per_rank);
    assert(status == DPU_API_SUCCESS && "dpu_get_nr_of_dpus_in failed");
    dpu_compute_time_t compute_time[nb_dpus_per_rank];
    struct dpu_t *dpu;
    unsigned int each_dpu = 0;
    DPU_FOREACH (rank, dpu) {
        dpu_transfer_matrix_add_dpu(
            dpu, matrix, &compute_time[each_dpu], sizeof(dpu_compute_time_t), devices->mram_compute_time_addr, 0);
        each_dpu++;
    }

    status = dpu_copy_from_dpus(rank, matrix);
    assert(status == DPU_API_SUCCESS && "dpu_copy_from_dpus failed");

#ifdef STATS_ON
    /* Collect stats */
    dpu_tasklet_stats_t tasklet_stats[nb_dpus_per_rank][NB_TASKLET_PER_DPU];
    for (unsigned int each_tasklet = 0; each_tasklet < NB_TASKLET_PER_DPU; each_tasklet++) {
        each_dpu = 0;
        DPU_FOREACH (rank, dpu) {
            dpu_transfer_matrix_add_dpu(dpu, matrix, &tasklet_stats[each_dpu][each_tasklet], sizeof(dpu_tasklet_stats_t),
                (mram_addr_t)(devices->mram_tasklet_stats_addr + each_tasklet * sizeof(dpu_tasklet_stats_t)), 0);
            each_dpu++;
        }
        status = dpu_copy_from_dpus(rank, matrix);
        assert(status == DPU_API_SUCCESS && "dpu_copy_from_dpus failed");
    }
#endif

    pthread_mutex_lock(&devices->log_mutex);
    fprintf(devices->log_file, "rank %u offset %u\n", rank_id, dpu_offset);
    each_dpu = 0;
    DPU_FOREACH (rank, dpu) {
        unsigned int this_dpu = each_dpu + dpu_offset + rank_id * nb_dpus_per_rank;
        fprintf(devices->log_file, "LOG DPU=%u TIME=%llu SEC=%.3f\n", this_dpu, (unsigned long long)compute_time[each_dpu],
            (float)compute_time[each_dpu] / CLOCK_PER_SECONDS);
#ifdef STATS_ON
        dpu_tasklet_stats_t agreagated_stats = {
            .nb_reqs = 0,
            .nb_nodp_calls = 0,
            .nb_odpd_calls = 0,
            .nb_results = 0,
            .mram_data_load = 0,
            .mram_result_store = 0,
            .mram_load = 0,
            .mram_store = 0,
            .nodp_time = 0ULL,
            .odpd_time = 0ULL,
        };
        for (unsigned int each_tasklet = 0; each_tasklet < NB_TASKLET_PER_DPU; each_tasklet++) {
            agreagated_stats.nb_reqs += tasklet_stats[each_dpu][each_tasklet].nb_reqs;
            agreagated_stats.nb_nodp_calls += tasklet_stats[each_dpu][each_tasklet].nb_nodp_calls;
            agreagated_stats.nb_odpd_calls += tasklet_stats[each_dpu][each_tasklet].nb_odpd_calls;
            agreagated_stats.nodp_time += (unsigned long long)tasklet_stats[each_dpu][each_tasklet].nodp_time;
            agreagated_stats.odpd_time += (unsigned long long)tasklet_stats[each_dpu][each_tasklet].odpd_time;
            agreagated_stats.nb_results += tasklet_stats[each_dpu][each_tasklet].nb_results;
            agreagated_stats.mram_data_load += tasklet_stats[each_dpu][each_tasklet].mram_data_load;
            agreagated_stats.mram_result_store += tasklet_stats[each_dpu][each_tasklet].mram_result_store;
            agreagated_stats.mram_load += tasklet_stats[each_dpu][each_tasklet].mram_load;
            agreagated_stats.mram_store += tasklet_stats[each_dpu][each_tasklet].mram_store;
        }
        fprintf(devices->log_file, "LOG DPU=%u REQ=%u\n", this_dpu, agreagated_stats.nb_reqs);
        fprintf(devices->log_file, "LOG DPU=%u NODP=%u\n", this_dpu, agreagated_stats.nb_nodp_calls);
        fprintf(devices->log_file, "LOG DPU=%u ODPD=%u\n", this_dpu, agreagated_stats.nb_odpd_calls);
        fprintf(devices->log_file, "LOG DPU=%u NODP_TIME=%llu\n", this_dpu, (unsigned long long)agreagated_stats.nodp_time);
        fprintf(devices->log_file, "LOG DPU=%u ODPD_TIME=%llu\n", this_dpu, (unsigned long long)agreagated_stats.odpd_time);
        fprintf(devices->log_file, "LOG DPU=%u RESULTS=%u\n", this_dpu, agreagated_stats.nb_results);
        fprintf(devices->log_file, "LOG DPU=%u DATA_IN=%u\n", this_dpu, agreagated_stats.mram_data_load);
        fprintf(devices->log_file, "LOG DPU=%u RESULT_OUT=%u\n", this_dpu, agreagated_stats.mram_result_store);
        fprintf(devices->log_file, "LOG DPU=%u LOAD=%u\n", this_dpu, agreagated_stats.mram_load);
        fprintf(devices->log_file, "LOG DPU=%u STORE=%u\n", this_dpu, agreagated_stats.mram_store);

#endif
        dpulog_read_for_dpu(dpu, devices->log_file);
        each_dpu++;
    }
    fflush(devices->log_file);
    pthread_mutex_unlock(&devices->log_mutex);
}

void dpu_try_get_results_and_log(
    unsigned int rank_id, __attribute__((unused)) unsigned int dpu_offset, devices_t *devices, dpu_result_out_t **result_buffer)
{
    dpu_api_status_t status;
    struct dpu_rank_t *rank = devices->ranks[rank_id];
    struct dpu_transfer_mram *matrix;
    status = dpu_transfer_matrix_allocate(rank, &matrix);
    assert(status == DPU_API_SUCCESS && "dpu_transfer_matrix_allocate failed");

    struct dpu_t *dpu;
    unsigned int each_dpu = 0;
    DPU_FOREACH(rank, dpu) {
        dpu_transfer_matrix_add_dpu(
            dpu, matrix, result_buffer[each_dpu], devices->mram_result_size, devices->mram_result_addr, 0);
        each_dpu++;
    }

    status = dpu_copy_from_dpus(rank, matrix);
    assert(status == DPU_API_SUCCESS && "dpu_copy_from_dpus failed");

    dpu_try_log(rank_id, dpu_offset, devices, matrix);
    dpu_transfer_matrix_free(rank, matrix);
}
