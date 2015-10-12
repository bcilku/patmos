/*
   Copyright 2015 Technical University of Denmark, DTU Compute. 
   All rights reserved.
   
   This file is part of the time-predictable VLIW processor Patmos.

   Redistribution and use in source and binary forms, with or without
   modification, are permitted provided that the following conditions are met:

      1. Redistributions of source code must retain the above copyright notice,
         this list of conditions and the following disclaimer.

      2. Redistributions in binary form must reproduce the above copyright
         notice, this list of conditions and the following disclaimer in the
         documentation and/or other materials provided with the distribution.

   THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDER ``AS IS'' AND ANY EXPRESS
   OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES
   OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN
   NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE FOR ANY
   DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
   (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
   LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND
   ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
   (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF
   THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.

   The views and conclusions contained in the software and documentation are
   those of the authors and should not be interpreted as representing official
   policies, either expressed or implied, of the copyright holder.
 */

/*
 * Message passing API
 * 
 * Author: Rasmus Bo Soerensen (rasmus@rbscloud.dk)
 *
 */

#include "mp.h"
#include "mp_internal.h"
#define TRACE_LEVEL INFO
#define DEBUG_ENABLE
#include "include/debug.h"

#define SINGLE_NOC 0
#define SINGLE_SHM 1
#define MULTI_NOC  2

#define IMPL MULTI_NOC

spd_t * mp_create_sport(const unsigned int chan_id, const direction_t direction_type,
              const coreid_t remote, const size_t sample_size) {
  if (chan_id >= MAX_CHANNELS || remote >= get_cpucnt()) {
    TRACE(FAILURE,TRUE,"Channel id or remote id is out of range: chan_id %d, remote: %d\n",chan_id,remote);
    return NULL;
  }

  spd_t * spd_ptr = mp_alloc(sizeof(spd_t));
  if (spd_ptr == NULL) {
    TRACE(FAILURE,TRUE,"Sampling port descriptor could not be allocated, SPM out of memory.\n");
    return NULL;
  }

  spd_ptr->direction_type = direction_type;
  spd_ptr->remote = remote;
  // Align the buffer size to double words and add the flag size
  spd_ptr->sample_size = DWALIGN(sample_size);

  spd_ptr->lock = initialize_lock(remote);
  TRACE(INFO,TRUE,"Initializing lock : %#08x\n",(unsigned int)spd_ptr->lock);

  if (spd_ptr->lock == NULL) {
    TRACE(FAILURE,TRUE,"Lock initialization failed\n");
    return NULL;
  }

  chan_info[chan_id].port_type = SAMPLING;
  if (direction_type == SOURCE) {
    #if IMPL == MULTI_NOC
      spd_ptr->reading = -1;
      spd_ptr->next = 0;
    #endif
    // src_desc_ptr must be set first inorder for
    // core 0 to see which cores are absent in debug mode
    chan_info[chan_id].src_spd_ptr = spd_ptr;
    if (chan_info[chan_id].src_spd_ptr == NULL) {
      TRACE(ERROR,TRUE,"src_spd_ptr written incorrectly\n");
      return NULL;
    }
    chan_info[chan_id].src_lock = spd_ptr->lock;

    #if IMPL == SINGLE_SHM
      // For shared memory buffer
      spd_ptr->read_shm_buf = malloc(DWALIGN(sample_size));
      chan_info[chan_id].src_addr = (volatile void _SPM *)spd_ptr->read_shm_buf;
      TRACE(ERROR,chan_info[chan_id].src_addr == NULL,"src_addr written incorrectly\n");
    #endif

    chan_info[chan_id].src_id = (char) get_cpuid();    
    TRACE(INFO,TRUE,"Initialization at sender done.\n");

  } else if (direction_type == SINK) {
    #if IMPL == MULTI_NOC
      spd_ptr->read_bufs = mp_alloc(DWALIGN(sample_size)*3);
      spd_ptr->newest = -1;
    #else
      spd_ptr->read_bufs = mp_alloc(DWALIGN(sample_size));
    #endif
    TRACE(INFO,TRUE,"Initialising SINK port buf_addr: %#08x\n",(unsigned int)spd_ptr->read_bufs);
    // sink_desc_ptr must be set first inorder for
    // core 0 to see which cores are absent in debug mode
    TRACE(INFO,TRUE,"SINK spd ptr: %#08x\n",(unsigned int)spd_ptr);
    chan_info[chan_id].sink_spd_ptr = spd_ptr;
    if (chan_info[chan_id].sink_spd_ptr == NULL) {
      TRACE(ERROR,TRUE,"src_spd_ptr written incorrectly\n");
      return NULL;
    }
    chan_info[chan_id].sink_lock = spd_ptr->lock;
    chan_info[chan_id].sink_addr = (volatile void _SPM *)spd_ptr->read_bufs;
    

    TRACE(ERROR,chan_info[chan_id].sink_addr == NULL,"sink_addr written incorrectly\n");
    if (spd_ptr->read_bufs == NULL) {
      TRACE(FAILURE,TRUE,"SPM allocation failed at SINK\n");
      return NULL;
    }
    chan_info[chan_id].sink_id = (char)get_cpuid();
    TRACE(INFO,TRUE,"Initialization at receiver done.\n");

  }

  return spd_ptr;
}

#if IMPL == SINGLE_SHM

int mp_read(spd_t * sport, volatile void _SPM * sample) {
  acquire_lock(sport->lock);
  // Since sample_size is in bytes and we want to copy 32 bit at the time we divide sample_size by 4
  unsigned itteration_count = (sport->sample_size + 4 - 1) / 4; // equal to ceil(sport->sample_size/4)
  inval_dcache();
  for (int i = 0; i < itteration_count; ++i) {
    ((int _SPM *)sample)[i] = ((volatile int *)sport->read_shm_buf)[i];
  }
  release_lock(sport->lock);

  return 0;

} 

int mp_write(spd_t * sport, volatile void _SPM * sample) {
  acquire_lock(sport->lock);
  // Since sample_size is in bytes and we want to copy 32 bit at the time we divide sample_size by 4
  unsigned itteration_count = (sport->sample_size + 4 - 1) / 4; // equal to ceil(sport->sample_size/4)
  for (int i = 0; i < itteration_count; ++i) {
    ((volatile int *)sport->read_shm_buf)[i] = ((int _SPM *)sample)[i];
  }
  release_lock(sport->lock);

  return 0;
} 

#elif IMPL == SINGLE_NOC

int mp_read(spd_t * sport, volatile void _SPM * sample) {
  acquire_lock(sport->lock);
  // Since sample_size is in bytes and we want to copy 32 bit at the time we divide sample_size by 4
  unsigned itteration_count = (sport->sample_size + 4 - 1) / 4; // equal to ceil(sport->sample_size/4)
  for (int i = 0; i < itteration_count; ++i) {
    ((int _SPM *)sample)[i] = ((volatile int _SPM *)sport->read_bufs)[i];
  }
  release_lock(sport->lock);

  return 0;

} 

int mp_write(spd_t * sport, volatile void _SPM * sample) {
  acquire_lock(sport->lock);
  noc_send(sport->remote,sport->read_bufs,sample,sport->sample_size);
  while(!noc_done(sport->remote));
  release_lock(sport->lock);

  return 0;
} 

#elif IMPL == MULTI_NOC

int mp_read(spd_t * sport, volatile void _SPM * sample) {
  int newest = 0;
  acquire_lock(sport->lock);
  // Read newest
  newest = (int)sport->newest;
  // Update reading
  if (newest >= 0) {
    noc_send( sport->remote,
              (void _SPM *)(((int)&(sport->remote_spd->reading)) ),
              (void _SPM *)&sport->newest,
              sizeof(sport->newest));
    while(!noc_done(sport->remote));
  }
  release_lock(sport->lock);

  if (newest < 0) {
    // No sample value has been written yet.
    return 1;
  }
  // Since sample_size is in bytes and we want to copy 32 bit at the time we divide sample_size by 4
  unsigned itteration_count = (sport->sample_size + 4 - 1) / 4; // equal to ceil(sport->sample_size/4)
  for (int i = 0; i < itteration_count; ++i) {
    ((int _SPM *)sample)[i] = ((volatile int _SPM *)(sport->read_bufs+
                              newest*sport->sample_size))[i];
  }

  return 0;

} 

int mp_write(spd_t * sport, volatile void _SPM * sample) {
  // Send the sample to the next buffer
  noc_send( sport->remote,
            (void _SPM *)( ((unsigned int)sport->read_bufs)+(((unsigned int)sport->next)*sport->sample_size) ),
            sample,
            sport->sample_size);
  while(!noc_done(sport->remote));
  // When the sample is sent take the lock
  unsigned int reading;
  acquire_lock(sport->lock);
  // Update newest
  noc_send( sport->remote,
            (void _SPM *)&(sport->remote_spd->newest),
            (void _SPM *)&sport->next,
            sizeof(sport->next));
  while(!noc_done(sport->remote));
  // update next based on the reading variable
  reading = (unsigned int)sport->reading;
  release_lock(sport->lock);

  sport->next++;
  if (sport->next >= 3) {
     sport->next = 0;
  }
  if (sport->next == reading) {
    sport->next++;
    if (sport->next >= 3) {
      sport->next = 0;
    }
  }
  

  return 0;
} 

#endif