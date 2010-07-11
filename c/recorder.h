/*
#   recorder.h: the recording part of the streaming module of idjc
#   Copyright (C) 2007-2009 Stephen Fairchild (s-fairchild@users.sourceforge.net)
#
#   This program is free software: you can redistribute it and/or modify
#   it under the terms of the GNU General Public License as published by
#   the Free Software Foundation, either version 2 of the License, or
#   (at your option) any later version.
#
#   This program is distributed in the hope that it will be useful,
#   but WITHOUT ANY WARRANTY; without even the implied warranty of
#   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
#   GNU General Public License for more details.
#
#   You should have received a copy of the GNU General Public License
#   along with this program in the file entitled COPYING.
#   If not, see <http://www.gnu.org/licenses/>.
*/

#ifndef RECORDER_H
#define RECORDER_H

#include <stdio.h>
#include "sourceclient.h"

enum record_mode { RM_STOPPED, RM_RECORDING, RM_PAUSED, RM_STOPPING };

struct recorder_vars
   {
   char *record_source;
   char *record_folder;
   char *pause_button;
   char *auto_pause_button;
   };

/* metadata logging (mp3 only): while the first structure logs title changes for
 * creating a table of contents in the id3 tag, the second logs changes to the
 * compression ratio to provide for the creation of a seek table in the Xing tag */
struct metadata_item
   {
   char *artist_title;
   int time_offset;
   int byte_offset;
   int time_offset_end;
   int byte_offset_end;
   struct metadata_item *next;
   };

struct metadata_item2
   {
   int start_offset_ms;
   int byte_offset;
   int finish_offset_ms;
   int size_bytes;
   int bit_rate;
   int sample_rate;
   struct metadata_item2 *next;
   };

struct recorder
   {
   struct watchdog_info watchdog_info;
   struct threads_info *threads_info;
   int numeric_id;              /* the identity of this recorder */
   pthread_t thread_h;          /* pthread handle for the recorder */
   int thread_terminate_f;      /* set this to cause the thread to exit */
   int stop_request;            /* control variables for various obvious things */
   int stop_pending;
   int pause_request;
   int pause_pending;
   int unpause_request;
   int unpause_pending;
   int initial_serial;          /* for syncing with the encoder */
   int final_serial;
   int recording_length_s;      /* time in whole seconds that are recorded */
   int recording_length_ms;
   double accumulated_time;     /* prior stream lengths are accumulated here */
   int bytes_written;           /* logs the current file size */
   struct encoder_op *encoder_op;       /* handle for getting input data */
   FILE *fp;
   char *pathname;              /* /path/to/filebeingsaved.[ogg/mp3] */
   char *title;         /* just the timestamp from the filename */
   enum record_mode record_mode;
   struct metadata_item *mi_first;      /* log mp3 song title changes */
   struct metadata_item *mi_last;
   struct metadata_item2 *mi2_first;    /* log mp3 block sizes and durations */
   struct metadata_item2 *mi2_last;
   int mp3_mode;                /* indicate we are recording an mp3 file */
   int is_vbr;                  /* frame length changed */
   int oldbitrate;
   int oldsamplerate;
   char first_mp3_header[4];
   };

struct recorder *recorder_init(struct threads_info *ti, int numeric_id);
void recorder_destroy(struct recorder *self);
int recorder_start(struct threads_info *ti, struct universal_vars *uv, void *other);
int recorder_stop(struct threads_info *ti, struct universal_vars *uv, void *other);
int recorder_pause(struct threads_info *ti, struct universal_vars *uv, void *other);
int recorder_unpause(struct threads_info *ti, struct universal_vars *uv, void *other);
int recorder_make_report(struct recorder *self);

#endif