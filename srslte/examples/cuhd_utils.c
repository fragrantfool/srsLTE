/**
 *
 * \section COPYRIGHT
 *
 * Copyright 2013-2014 The srsLTE Developers. See the
 * COPYRIGHT file at the top-level directory of this distribution.
 *
 * \section LICENSE
 *
 * This file is part of the srsLTE library.
 *
 * srsLTE is free software: you can redistribute it and/or modify
 * it under the terms of the GNU Lesser General Public License as
 * published by the Free Software Foundation, either version 3 of
 * the License, or (at your option) any later version.
 *
 * srsLTE is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Lesser General Public License for more details.
 *
 * A copy of the GNU Lesser General Public License can be found in
 * the LICENSE file in the top-level directory of this distribution
 * and at http://www.gnu.org/licenses/.
 *
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>
#include <math.h>
#include <sys/time.h>

#include <unistd.h>

#include "srslte/srslte.h"
#include "cuhd_utils.h"


#ifndef DISABLE_UHD
#include "srslte/cuhd/cuhd.h"

int cuhd_recv_wrapper_cs(void *h, void *data, uint32_t nsamples, srslte_timestamp_t *t) {
  DEBUG(" ----  Receive %d samples  ---- \n", nsamples);
  return cuhd_recv(h, data, nsamples, 1);
}

/** This function is simply a wrapper to the ue_cell_search module for cuhd devices 
 * Return 1 if the MIB is decoded, 0 if not or -1 on error. 
 */
int cuhd_mib_decoder(void *uhd, uint32_t max_nof_frames, srslte_cell_t *cell) {
  int ret = SRSLTE_ERROR; 
  srslte_ue_mib_sync_t ue_mib; 
  uint8_t bch_payload[SRSLTE_BCH_PAYLOAD_LEN];

  if (srslte_ue_mib_sync_init(&ue_mib, cell->id, cell->cp, cuhd_recv_wrapper_cs, uhd)) {
    fprintf(stderr, "Error initiating srslte_ue_mib_sync\n");
    goto clean_exit; 
  }
  
  srslte_ue_sync_start_agc(&ue_mib.ue_sync, cuhd_set_rx_gain);

  int srate = srslte_sampling_freq_hz(SRSLTE_UE_MIB_NOF_PRB);
  INFO("Setting sampling frequency %.2f MHz for PSS search\n", (float) srate/1000000);
  cuhd_set_rx_srate(uhd, (float) srate);
  
  INFO("Starting receiver...\n", 0);
  cuhd_start_rx_stream(uhd);
    
  /* Find and decody MIB */
  ret = srslte_ue_mib_sync_decode(&ue_mib, max_nof_frames, bch_payload, &cell->nof_ports, NULL); 
  if (ret < 0) {
    fprintf(stderr, "Error decoding MIB\n");
    goto clean_exit; 
  }
  if (ret == 1) {
    srslte_pbch_mib_unpack(bch_payload, cell, NULL);
  }

clean_exit: 

  cuhd_stop_rx_stream(uhd);
  srslte_ue_mib_sync_free(&ue_mib);

  return ret; 
}

/** This function is simply a wrapper to the ue_cell_search module for cuhd devices 
 */
int cuhd_cell_search(void *uhd, cell_search_cfg_t *config, 
                     int force_N_id_2, srslte_cell_t *cell) 
{
  int ret = SRSLTE_ERROR; 
  srslte_ue_cellsearch_t cs; 
  srslte_ue_cellsearch_result_t found_cells[3];

  bzero(found_cells, 3*sizeof(srslte_ue_cellsearch_result_t));
    
  if (srslte_ue_cellsearch_init(&cs, cuhd_recv_wrapper_cs, uhd)) {
    fprintf(stderr, "Error initiating UE cell detect\n");
    return SRSLTE_ERROR; 
  }
  
  if (config->max_frames_pss) {
    srslte_ue_cellsearch_set_nof_frames_to_scan(&cs, config->max_frames_pss);
  }
  if (config->threshold) {
    srslte_ue_cellsearch_set_threshold(&cs, config->threshold);
  }
  
  srslte_ue_sync_start_agc(&cs.ue_sync, cuhd_set_rx_gain);

  INFO("Setting sampling frequency %.2f MHz for PSS search\n", SRSLTE_CS_SAMP_FREQ/1000000);
  cuhd_set_rx_srate(uhd, SRSLTE_CS_SAMP_FREQ);
  
  INFO("Starting receiver...\n", 0);
  cuhd_start_rx_stream(uhd);

  /* Find a cell in the given N_id_2 or go through the 3 of them to find the strongest */
  uint32_t max_peak_cell = 0;
  if (force_N_id_2 >= 0) {
    ret = srslte_ue_cellsearch_scan_N_id_2(&cs, force_N_id_2, &found_cells[force_N_id_2]);
    max_peak_cell = force_N_id_2;
  } else {
    ret = srslte_ue_cellsearch_scan(&cs, found_cells, &max_peak_cell); 
  }
  if (ret < 0) {
    fprintf(stderr, "Error searching cell\n");
    return SRSLTE_ERROR;
  } else if (ret == 0) {
    fprintf(stderr, "Could not find any cell in this frequency\n");
    return SRSLTE_SUCCESS;
  }
  
  // Save result 
  if (cell) {
    cell->id = found_cells[max_peak_cell].cell_id;
    cell->cp = found_cells[max_peak_cell].cp; 
  }

  cuhd_stop_rx_stream(uhd);
  srslte_ue_cellsearch_free(&cs);

  return ret; 
}


/* Finds a cell and decodes MIB from the PBCH. 
 * Returns 1 if the cell is found and MIB is decoded successfully. 
 * 0 if no cell was found or MIB could not be decoded, 
 * -1 on error
 */
int cuhd_search_and_decode_mib(void *uhd, cell_search_cfg_t *config, int force_N_id_2, srslte_cell_t *cell) 
{
  int ret = SRSLTE_ERROR; 
  
  printf("Searching for cell...\n");
  ret = cuhd_cell_search(uhd, config, force_N_id_2, cell);
  if (ret > 0) {
    printf("Decoding PBCH for cell %d (N_id_2=%d)\n", cell->id, cell->id%3);        
    ret = cuhd_mib_decoder(uhd, config->max_frames_pbch, cell);
    if (ret < 0) {
      fprintf(stderr, "Could not decode PBCH from CELL ID %d\n", cell->id);
      return SRSLTE_ERROR;
    }    
  }
  return ret;
}


#endif
