/*
#   encoder.c: the encoder framework for the streaming module of idjc
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

#include "../config.h"
#include "gnusource.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <stdint.h>
#include <jack/ringbuffer.h>
#include "sourceclient.h"
#include "live_ogg_encoder.h"
#include "live_mp3_encoder.h"
#include "live_oggflac_encoder.h"
#include "live_oggspeex_encoder.h"
#include "bsdcompat.h"
#ifdef DYN_LAME
#include "dyn_lame.h"
#endif

#define RS_INPUT_SAMPLES 512

typedef jack_default_audio_sample_t sample_t;

static const size_t rb_n_samples = 10000;       /* maximum number of samples to hold in the ring buffer */
static uint32_t encoder_packet_magic_number = 'I' << 24 | 'D' << 16 | 'J' << 8 | 'C';

int encoder_init_lame(struct threads_info *ti, struct universal_vars *uv, void *param)
   {
   int l = 1;

#ifdef DYN_LAME
   l = dyn_lame_init();
#endif

   printf("idjcsc: lame_available=%d\n", l);
   fflush(stdout);
   if (ferror(stdout))
      return FAILED;
   return SUCCEEDED;
   }

static enum data_format encoder_get_data_format(char *format, char *source)
   {
   if (!strcmp(format, "mp3"))
      {
      if (!strcmp(source, "jack"))
         return DF_JACK_MP3;
      else
         return DF_FILE_MP3;
      }
   if (!strcmp(format, "ogg"))
      {
      if (!strcmp(source, "jack"))
         return DF_JACK_OGG;
      else
         return DF_FILE_OGG;
      }
   return DF_UNHANDLED;
   }

static int encoder_get_resample_mode(char *rm_string)
   {
   if (!strcmp(rm_string, "fastest"))
      return SRC_LINEAR;
   if (!strcmp(rm_string, "fast"))
      return SRC_SINC_FASTEST;
   if (!strcmp(rm_string, "high"))
      return SRC_SINC_MEDIUM_QUALITY;
   if (!strcmp(rm_string, "highest"))
      return SRC_SINC_BEST_QUALITY;
   fprintf(stderr, "encoder_get_resample_mode: unknown resample mode %s\n", rm_string);
   return -1;
   }

static void encoder_free_input_ringbuffers(struct encoder *self)
   {
   struct timespec ms10 = { 0, 10000000 };
   
   if (self->jack_dataflow_control == JD_ON)
      self->jack_dataflow_control = JD_FLUSH;
   while (self->jack_dataflow_control != JD_OFF)
      nanosleep(&ms10, NULL);
   
   if (self->input_rb[0])
      jack_ringbuffer_free(self->input_rb[0]);
   if (self->input_rb[1])
      jack_ringbuffer_free(self->input_rb[1]);
   self->input_rb[0] = self->input_rb[1] = NULL;
   }

static void encoder_free_resampler(struct encoder *self)
   {
   int i;
   
   for (i = 0; i < 2; i++)
      if (self->src_state[i])
         {
         src_delete(self->src_state[i]);
         self->src_state[i] = NULL;
         }
   }

static void encoder_plugin_terminate(struct encoder *self)
   {
   struct timespec ms10 = { 0, 10000000 };
      
   self->run_request_f = FALSE;
   if (self->encoder_state != ES_STOPPED)
      fprintf(stderr, "encoder_plugin_terminate: waiting for encoder to finish\n");
   while (self->encoder_state != ES_STOPPED)
      nanosleep(&ms10, NULL);
   }

static void encoder_unlink(struct encoder *self)
   {
   encoder_plugin_terminate(self);
   encoder_free_input_ringbuffers(self);
   encoder_free_resampler(self);
   }

static long encoder_input_rb_mono_downmix(jack_ringbuffer_t **rb, float *bptr, int max_samples)
   {
   jack_ringbuffer_data_t rbvec0[2], rbvec1[2];
   sample_t *ch0, *ch1;
   int transition, i;
   long n_samples;

   jack_ringbuffer_get_read_vector(rb[0], rbvec0);
   jack_ringbuffer_get_read_vector(rb[1], rbvec1);
   n_samples = (rbvec1[0].len + rbvec1[1].len) / sizeof (sample_t);
   if (n_samples > max_samples)
      n_samples = max_samples;
   /* transition is the point at the end of the ringbuffer where we must wrap around */
   if((transition = rbvec0[0].len / sizeof (sample_t)) > n_samples)
      transition = n_samples;
   ch0 = (sample_t *)rbvec0[0].buf;     /* set channel 0 and 1 pointers */
   ch1 = (sample_t *)rbvec1[0].buf;
   for (i = 0; i < transition; i++)
      *bptr++ = (*ch0++ + *ch1++) * 0.5F;       /* copy up to the transition */
   ch0 = (sample_t *)rbvec0[1].buf;     /* set pointers for segment 2 */
   ch1 = (sample_t *)rbvec1[1].buf;
   for (i = transition; i < n_samples; i++)     /* do the second segment if relevant */
      *bptr++ = (*ch0++ + *ch1++) * 0.5F;
   jack_ringbuffer_read_advance(rb[0], n_samples * sizeof (sample_t));
   jack_ringbuffer_read_advance(rb[1], n_samples * sizeof (sample_t));
   return n_samples;
   }
   
static long encoder_input_rb_stereo(jack_ringbuffer_t **rb, float **dest, long max_samples)
   {
   long n_samples;
   int i;

   n_samples = jack_ringbuffer_read_space(rb[1]) / sizeof (sample_t);
   if (n_samples > max_samples)
      n_samples = max_samples;
   for (i = 0; i < 2; i++, rb++, dest++)
      jack_ringbuffer_read(*rb, (char *)*dest, n_samples * sizeof (sample_t));
   return n_samples;
   }
   
static long encoder_input_rb_one_channel(jack_ringbuffer_t **rb, float **dest, long max_samples, int c)
   {
   long n_samples;

   n_samples = jack_ringbuffer_read_space(rb[c]) / sizeof (sample_t);
   if (n_samples > max_samples)
      n_samples = max_samples;
   jack_ringbuffer_read(rb[c], (char *)dest[c], n_samples * sizeof (sample_t));
   return n_samples;
   }

static long encoder_resampler_get_data(void *cb_data, float **data)
   {
   struct encoder *encoder = cb_data;
   long n_samples;
   
   if (encoder->rs_channel >= 0)
      {
      n_samples = encoder_input_rb_one_channel(encoder->input_rb, encoder->rs_input, RS_INPUT_SAMPLES, encoder->rs_channel);
      *data = encoder->rs_input[encoder->rs_channel];
      }
   else
      {
      n_samples = encoder_input_rb_mono_downmix(encoder->input_rb, encoder->rs_input[0], RS_INPUT_SAMPLES);
      *data = encoder->rs_input[0];
      }
   return (long)n_samples;
   }
  
struct encoder_ip_data *encoder_get_input_data(struct encoder *encoder, int min_samples_needed, int max_samples, float **caller_supplied_buffer)
   {
   struct encoder_ip_data *id;
   int samples_available, i;
   
   if (!(id = calloc(1, sizeof (struct encoder_ip_data))))
      {
      fprintf(stderr, "encoder_get_input_data: malloc failure\n");
      return NULL;
      }
   id->channels = encoder->n_channels;
   if (caller_supplied_buffer)
      {
      /* link callers own buffer into the encoder_input_data structure */
      for (i = 0; i < encoder->n_channels; i++)
         id->buffer[i] = caller_supplied_buffer[i];
      id->caller_supplied_buffer = TRUE;
      }
   else
      {
      /* make our own buffer */
      for (i = 0; i < encoder->n_channels; i++)
         if (!(id->buffer[i] = malloc(max_samples * sizeof (sample_t))))
            {
            fprintf(stderr, "encoder_get_input_data: malloc failure\n");
            goto no_data;
            }
      }
   if (!encoder->resample_f)
      {
      if (jack_ringbuffer_read_space(encoder->input_rb[1]) / sizeof (sample_t) < min_samples_needed)
         goto no_data;
      if (encoder->n_channels == 2)
         id->qty_samples = encoder_input_rb_stereo(encoder->input_rb, id->buffer, max_samples);
      else
         id->qty_samples = encoder_input_rb_mono_downmix(encoder->input_rb, id->buffer[0], max_samples);
      }
   else
      {                 /* handle the resampling condition */
      /* note 128 samples are held back to make sure the resampler gives the full number of samples on both reads */
      samples_available = (int)(jack_ringbuffer_read_space(encoder->input_rb[1]) / sizeof (sample_t)) * encoder->sr_conv_ratio - 128.0;
      if (samples_available > max_samples)
         samples_available = max_samples;
      if (samples_available < min_samples_needed)
         goto no_data;
      if (encoder->n_channels == 2)
         {
         encoder->rs_channel = 0;
         id->qty_samples = (size_t)src_callback_read(encoder->src_state[0], encoder->sr_conv_ratio, samples_available, id->buffer[0]);
         encoder->rs_channel = 1;
         src_callback_read(encoder->src_state[1], encoder->sr_conv_ratio, id->qty_samples, id->buffer[1]);
         }
      else
         {
         encoder->rs_channel = -1;
         id->qty_samples = (size_t)src_callback_read(encoder->src_state[0], encoder->sr_conv_ratio, samples_available, id->buffer[0]);
         }
      if (id->qty_samples == 0)
         goto no_data;
      }
   return id;
   no_data:
   encoder_ip_data_free(id);
   return NULL;
   }
   
void encoder_ip_data_free(struct encoder_ip_data *id)
   {
   int i;
   
   if (!id->caller_supplied_buffer)
      for (i = 0; i < id->channels; i++)
         if (id->buffer[i])
            free(id->buffer[i]);
   free(id);
   }

/* note encoder.mutex must be locked before helper threads can safely traverse 
   encoder.output_chain to find the op structure to pass to this function */
size_t encoder_write_packet(struct encoder_op *op, struct encoder_op_packet *packet)
   {
   size_t packet_size, written;
    
   packet->header.magic = encoder_packet_magic_number;
   packet->header.serial = op->encoder->oggserial;
   packet_size = sizeof packet->header + packet->header.data_size;
   while (packet_size > jack_ringbuffer_write_space(op->packet_rb))
      {
      if (jack_ringbuffer_read_space(op->packet_rb) == 0)
         {
         fprintf(stderr, "encoder_write_packet: packet too big to fit in the ringbuffer\n");
         return 0;
         }
      encoder_client_free_packet(encoder_client_get_packet(op)); /* flush stale packets */
      op->performance_warning_indicator = PW_AUDIO_DATA_DROPPED;
      }
   pthread_mutex_lock(&op->mutex);
   written = jack_ringbuffer_write(op->packet_rb, (char *)&packet->header, sizeof packet->header);
   written += jack_ringbuffer_write(op->packet_rb, (char *)packet->data, packet->header.data_size);
   pthread_mutex_unlock(&op->mutex);
   return written;
   }
   
void encoder_write_packet_all(struct encoder *encoder, struct encoder_op_packet *packet)
   {
   struct encoder_op *iter;
   struct timespec ms10 = { 0, 10000000 };
   
   while (pthread_mutex_trylock(&encoder->mutex))
      nanosleep(&ms10, NULL);
   for (iter = encoder->output_chain; iter; iter = iter->next)
      encoder_write_packet(iter, packet);
   pthread_mutex_unlock(&encoder->mutex);
   }

struct encoder_op_packet *encoder_client_get_packet(struct encoder_op *op)
   {
   struct encoder_op_packet *packet;
   
   pthread_mutex_lock(&op->mutex);
   if (jack_ringbuffer_read_space(op->packet_rb) >= sizeof (struct encoder_op_packet_header))
      {
      if (!(packet = calloc(1, sizeof (struct encoder_op_packet))))
         {
         fprintf(stderr, "encoder_client_get_packet: malloc failure\n");
         goto unlock;
         }
      jack_ringbuffer_read(op->packet_rb, (char *)packet, sizeof (struct encoder_op_packet_header));
      if (packet->header.magic != encoder_packet_magic_number)
         {
         fprintf(stderr, "encoder_client_get_packet: magic number missing\n");
         free(packet);
         goto unlock;
         }
      if (jack_ringbuffer_read_space(op->packet_rb) < packet->header.data_size)
         {
         fprintf(stderr, "encoder_client_get_packet: packet header specifying more data than can fit in the buffer\n");
         free(packet);
         goto unlock;
         }   
      if (packet->header.data_size)
         {
         if (!(packet->data = malloc(packet->header.data_size)))
            {
            fprintf(stderr, "encoder_client_get_packet: malloc failure for data buffer\n");
            free(packet);
            goto unlock;
            }
         jack_ringbuffer_read(op->packet_rb, packet->data, packet->header.data_size);
         }
      pthread_mutex_unlock(&op->mutex);
      return packet;
      }
   unlock:
   pthread_mutex_unlock(&op->mutex);
   return NULL;
   }
   
void encoder_client_free_packet(struct encoder_op_packet *packet)
   {
   if (packet->data)
      free(packet->data);
   free(packet);
   }

int encoder_client_set_flush(struct encoder_op *op)
   {
   struct encoder *encoder = op->encoder;
   struct timespec ns1 = { 0, 1 };
   int serial;
   
   while (pthread_mutex_trylock(&encoder->flush_mutex))
      nanosleep(&ns1, NULL);
   serial = encoder->oggserial;
   encoder->flush = TRUE;
   pthread_mutex_unlock(&encoder->flush_mutex);
   return serial;
   }

/* this is called from a recipient thread to obtain a handle for getting data */ 
/* the numeric_id is the encoder that is requested */
struct encoder_op *encoder_register_client(struct threads_info *ti, int numeric_id)
   {
   struct encoder *enc;
   struct encoder_op *op;
   struct timespec ms10 = { 0, 10000000 };
   
   if (numeric_id >= ti->n_encoders || numeric_id < 0)
      {
      fprintf(stderr, "encoder_register_client: invalid encoder numeric_id %d\n", numeric_id);
      return NULL;
      }
   if (!(op = calloc(1, sizeof (struct encoder_op))))
      {
      fprintf(stderr, "encoder_register_client: malloc failure\n");
      return NULL;
      }
   if (!(op->packet_rb = jack_ringbuffer_create(24000)))
      {
      fprintf(stderr, "encoder_register_client: malloc failure\n");
      free(op);
      return NULL;
      }
   enc = ti->encoder[numeric_id];
   op->encoder = enc;
   pthread_mutex_init(&op->mutex, NULL);
   while (pthread_mutex_trylock(&op->encoder->mutex))
      nanosleep(&ms10, NULL);
   op->next = enc->output_chain;
   enc->output_chain = op;
   enc->client_count++;
   pthread_mutex_unlock(&op->encoder->mutex);
   return op;
   }
   
void encoder_unregister_client(struct encoder_op *op)
   {
   struct encoder_op *iter;
   struct timespec ms10 = { 0, 10000000 };      /* ten milliseconds */
   
   fprintf(stderr, "encoder_unregister_client called\n");
   while (pthread_mutex_trylock(&op->encoder->mutex))
      nanosleep(&ms10, NULL);
   if ((iter = op->encoder->output_chain) == op)
      op->encoder->output_chain = op->next;
   else
      {
      while (iter->next != op)
         iter = iter->next;
      iter->next = op->next;
      }
   op->encoder->client_count--;
   pthread_mutex_unlock(&op->encoder->mutex);
   pthread_mutex_destroy(&op->mutex);
   jack_ringbuffer_free(op->packet_rb);
   free(op);
   fprintf(stderr, "encoder_unregister_client finished\n");
   }

void *encoder_main(void *args)
   {
   struct encoder *self = args;
   struct timespec ms10 = { 0, 10000000 };      /* ten milliseconds */

   while(!self->thread_terminate_f)
      {
      pthread_mutex_lock(&self->flush_mutex);
      self->watchdog_info.tick++;
      switch(self->encoder_state)
         {
         case ES_STOPPED:
            break;
         case ES_STARTING:
         case ES_PAUSED:
         case ES_RUNNING:
         case ES_STOPPING:
            self->run_encoder(self);
            break;
         }
      pthread_mutex_unlock(&self->flush_mutex);
      nanosleep(&ms10, NULL);
      }
   return NULL;
   }

int encoder_start(struct threads_info *ti, struct universal_vars *uv, void *other)
   {
   struct encoder *self = ti->encoder[uv->tab];
   struct encoder_vars *ev = other;
   struct timespec ms10 = { 0, 10000000 };
   int (*encoder_init)(struct encoder *, struct encoder_vars *);
   int i, resample_mode, error;

   if (self->encoder_state != ES_STOPPED)
      {
      fprintf(stderr, "encoder_start: encoder state out of control - shouldn't be marked as running\n");
      goto failed;
      }
   self->data_format = encoder_get_data_format(ev->format, ev->encode_source);
   switch (self->data_format)
      {
      case DF_JACK_MP3:
         encoder_init = live_mp3_encoder_init;
         break;
      case DF_JACK_OGG:
         if (!strcmp(ev->subformat, "vorbis"))
            {
            encoder_init = live_ogg_encoder_init;
            break;
            }
#ifdef HAVE_OGGFLAC
         if (!strcmp(ev->subformat, "flac"))
            {
            encoder_init = live_oggflac_encoder_init;
            break;
            }
#endif
#ifdef HAVE_SPEEX
         if (!strcmp(ev->subformat, "speex"))
            {
            encoder_init = live_oggspeex_encoder_init;
            break;
            }
#endif
      /*case DF_FILE_MP3:
         encoder_init = file_mp3_encoder_init;
         break;
      case DF_FILE_OGG:
         encoder_init = file_ogg_encoder_init;
         break;*/
         fprintf(stderr, "encoder_start: unhandled ogg format %s\n", ev->subformat);
         goto failed;
      case DF_UNHANDLED:
      default:
         fprintf(stderr, "encoder_start: unhandled file format %s:%s\n", ev->format, ev->encode_source);
         goto failed;
      }
   self->performance_warning_indicator = PW_OK;
   self->samplerate = (long)self->threads_info->audio_feed->sample_rate;
   self->target_samplerate = atol(ev->sample_rate);
   self->resample_f = !(self->samplerate == self->target_samplerate);
   self->sr_conv_ratio = (double)self->target_samplerate / (double)self->samplerate;
   self->bitrate = atoi(ev->bit_rate);
   self->n_channels = strcmp(ev->stereo, "mono") ? 2 : 1;
   self->new_metadata = TRUE;
   if (self->resample_f)
      {
      fprintf(stderr, "encoder_start: initiating resampler(s)\n");
      resample_mode = encoder_get_resample_mode(ev->resample_quality);
      for (i = 0; i < self->n_channels; i++)
         {
         if (!(self->src_state[i] = src_callback_new(encoder_resampler_get_data, resample_mode, 1, &error, self)))
            goto failed;
         src_set_ratio(self->src_state[i], self->sr_conv_ratio);
         }
      }
   else
      fprintf(stderr, "encoder_start: resampler will not be used\n");
   if (encoder_init(self, ev))
      {
      switch(self->data_format)
         {
         case DF_JACK_OGG:
         case DF_JACK_MP3:
            self->input_rb[0] = jack_ringbuffer_create(rb_n_samples * sizeof (sample_t));
            self->input_rb[1] = jack_ringbuffer_create(rb_n_samples * sizeof (sample_t));
            if (!(self->input_rb[0] && self->input_rb[1]))
               {
               fprintf(stderr, "encoder_start: jack ringbuffer creation failure\n");
               goto failed;
               }
            self->jack_dataflow_control = JD_ON;
            break;
         case DF_FILE_OGG:
         case DF_FILE_MP3:
         default:
            break;
         }
      self->run_request_f = TRUE;
      self->encoder_state = ES_STARTING;
      while (self->encoder_state == ES_STARTING)
         nanosleep(&ms10, NULL);
      if (self->encoder_state == ES_STOPPED)
         {
         fprintf(stderr, "encoder_start: encoder failed during initialisation\n");
         goto failed;
         }
      fprintf(stderr, "encoder_start: successfully started the encoder\n");
      return SUCCEEDED;
      }
   failed:
   encoder_unlink(self);
   fprintf(stderr, "encoder_start: failed to start the encoder\n");
   return FAILED;
   }

int encoder_stop(struct threads_info *ti, struct universal_vars *uv, void *other)
   {
   struct encoder *self = ti->encoder[uv->tab];
   
   encoder_unlink(self);
   if (self->output_chain)
      fprintf(stderr, "encoder_stop: function has been called with encoder_op objects still attached\n");
   fprintf(stderr, "encoder_stop: encoder is stopped\n");
   return SUCCEEDED;
   }
 
int encoder_update(struct threads_info *ti, struct universal_vars *uv, void *other)
   {
   struct encoder *self = ti->encoder[uv->tab];
   
   encoder_unlink(self);
   return encoder_start(ti, uv, other);
   }
 
int encoder_new_metadata(struct threads_info *ti, struct universal_vars *uv, void *other)
   {
   struct encoder *self;
   struct encoder_vars *ev = other;
   
   if (uv->tab == -1)
      {
      for (uv->tab = 0; uv->tab < ti->n_encoders; uv->tab++)
         if (!(encoder_new_metadata(ti, uv, other)))
            return FAILED;
      }
   else
      {
      self = ti->encoder[uv->tab];
      pthread_mutex_lock(&self->metadata_mutex);
      self->new_metadata = FALSE;
      if (self->artist)
         free(self->artist);
      if (self->title)
         free(self->title);
      if (self->artist_title)
         free(self->artist_title);
      if (self->artist_title_mp3)
         free(self->artist_title_mp3);
      if (ev->artist)
         self->artist = strdup(ev->artist);
      else
         self->artist = strdup("");
      if (ev->title)
         self->title = strdup(ev->title);
      else
         self->title = strdup("");
      if (ev->artist_title)
         self->artist_title = strdup(ev->artist_title);
      else
         self->artist_title = strdup("");
      if (ev->artist_title_mp3)
         self->artist_title_mp3 = strdup(ev->artist_title_mp3);
      else
         self->artist_title_mp3 = strdup("");
      if (!(self->artist && self->title && self->artist_title && self->artist_title_mp3))
         {
         pthread_mutex_unlock(&self->metadata_mutex);
         fprintf(stderr, "encoder_new_metadata: malloc failure\n");
         return FAILED;
         }
      self->new_metadata = TRUE;
      pthread_mutex_unlock(&self->metadata_mutex);
      return SUCCEEDED;
      }
   return SUCCEEDED;
   }

int encoder_new_metaformat(struct threads_info *ti, struct universal_vars *uv, void *other)
   {
   struct encoder *self = ti->encoder[uv->tab];
   struct encoder_vars *ev = other;

   pthread_mutex_lock(&self->metadata_mutex);
   self->new_metadata = FALSE;
   if (self->metaformat)
      free(self->metaformat);
   if (self->metaformat_mp3)
      free(self->metaformat_mp3);
   self->metaformat = ev->metaformat;
   ev->metaformat = NULL;
   self->metaformat_mp3 = ev->metaformat_mp3;
   ev->metaformat_mp3 = NULL;
   if (!self->metaformat)
      self->metaformat = strdup("");
   if (!self->metaformat_mp3)
      self->metaformat_mp3 = strdup("");
   self->new_metadata = TRUE;
   pthread_mutex_unlock(&self->metadata_mutex);
   return SUCCEEDED;
   }

struct encoder *encoder_init(struct threads_info *ti, int numeric_id)
   {
   struct encoder *self;
   
   if (!(self = calloc(1, sizeof (struct encoder))))
      {
      fprintf(stderr, "encoder_init: malloc failure\n");
      return NULL;
      }
   self->rs_input[0] = malloc(RS_INPUT_SAMPLES * sizeof (sample_t));
   self->rs_input[1] = malloc(RS_INPUT_SAMPLES * sizeof (sample_t));
   if (!(self->rs_input[0] && self->rs_input[1]))
      {
      fprintf(stderr, "encoder_init: malloc failure\n");
      free(self);
      return NULL;
      }
   self->threads_info = ti;
   self->numeric_id = numeric_id;
   self->artist = strdup("");
   self->title = strdup("");
   self->artist_title = strdup("");
   self->artist_title_mp3 = strdup("");
   self->metaformat = strdup("%s");
   self->metaformat_mp3 = strdup("%s");
   while ((self->oggserial = rand()) + 20000 < 0 || self->oggserial < 100);
   pthread_mutex_init(&self->mutex, NULL);
   pthread_mutex_init(&self->metadata_mutex, NULL);
   pthread_mutex_init(&self->flush_mutex, NULL);
   if (pthread_create(&self->thread_h, NULL, encoder_main, self))
      {
      fprintf(stderr, "encoder_init: pthread_create call failed\n");
      return NULL;
      }
   /* the input ringbuffer will be allocated when the encoder is started */
   return self;
   }

void encoder_destroy(struct encoder *self)
   {
   self->thread_terminate_f = TRUE;
   pthread_join(self->thread_h, NULL);
   pthread_mutex_destroy(&self->mutex);
   pthread_mutex_destroy(&self->metadata_mutex);
   pthread_mutex_destroy(&self->flush_mutex);
   if (self->rs_input[0])
      free(self->rs_input[0]);
   if (self->rs_input[1])
      free(self->rs_input[1]);
   if (self->metaformat)
      free(self->metaformat);
   if (self->metaformat_mp3)
      free(self->metaformat_mp3);
   if (self->artist)
      free(self->artist);
   if (self->title)
      free(self->title);
   if (self->artist_title)
      free(self->artist_title);
   if (self->artist_title_mp3)
      free(self->artist_title_mp3);
   free(self);
   }
