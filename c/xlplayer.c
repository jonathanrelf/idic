/*
#   xlplayer.c: player decoder module for idjc
#   Copyright (C) 2006-2009 Stephen Fairchild (s-fairchild@users.sourceforge.net)
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

#include "gnusource.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <unistd.h>
#include <ctype.h>
#include <jack/jack.h>
#include <jack/ringbuffer.h>
#include <samplerate.h>

#include "xlplayer.h"
#include "mp3dec.h"
#include "oggdec.h"
#include "flacdecode.h"
#include "sndfiledecode.h"
#include "avcodecdecode.h"
#include "bsdcompat.h"

#define TRUE 1
#define FALSE 0

#define PBSPEED_INPUT_SAMPLE_SIZE 256
#define PBSPEED_INPUT_BUFFER_SIZE (PBSPEED_INPUT_SAMPLE_SIZE * sizeof (float))

typedef jack_default_audio_sample_t sample_t;

extern int have_mad, fb_size;

/* make_audio_to_float: convert the audio to the format used by jack and libsamplerate */
float *xlplayer_make_audio_to_float(struct xlplayer *self, float *buffer, uint8_t *data, int num_samples, int bits_per_sample, int num_channels)
   {
   int num_bytes;
   int i;
   uint32_t msb_mask;
   uint32_t neg_mask;
   uint32_t holder;
   uint32_t mult;
   float *fptr = buffer;
   float fscale;
   const float half_randmax = (float)(RAND_MAX >> 1);
   float dscale;

   msb_mask = 1UL << (bits_per_sample - 1);             /* negative number detector */
   neg_mask = (uint32_t)((~0UL) << (bits_per_sample));  /* negative number maker */
   fscale = 1.0F/(float)msb_mask;                       /* multiplier to make the floating point range -1 to +1 */
   dscale = 0.25F / half_randmax * fscale;

   if (bits_per_sample > 32)
      {
      memset(buffer, 0, sizeof (sample_t) * num_samples * num_channels);
      }
   else
      {
      while (num_samples--)
         {
         for (i = 0; i < num_channels; i++)
            {
            for (num_bytes = (bits_per_sample + 7) >> 3, mult = 1, holder = 0; num_bytes--; mult <<=8)
               {
               holder |= ((uint32_t)*data++) * mult;
               }
            if (holder & msb_mask)
               holder |= neg_mask;
            if (self->dither && bits_per_sample < 20)
               /* adds triangular dither */
               *fptr++ = (((float)(int32_t)holder) * fscale) + 
               (((((float)rand_r(&self->seed)) - half_randmax) +
               (((float)rand_r(&self->seed)) - half_randmax)) * dscale); 
            else
               *fptr++ = ((float)((int32_t)holder)) * fscale;
            }
         }
      }
   return buffer;
   }

/* get_next_gain: compute the gain of the next sample */
/* used to fade in the audio when not starting from the beginning */
sample_t xlplayer_get_next_gain(struct xlplayer *self)
   {
   float val;
   extern float *fade_table;

   if (self->fadein_index >= 0)
      {
      val = fade_table[self->fadein_index];
      self->fadein_index -= self->fadeinstep;
      return val * self->gain;
      }
   else
      return self->gain;
   }

/* xlplayer_demux_channel_data: this is where down/upmixing is performed - audio split to 2 channels */
void xlplayer_demux_channel_data(struct xlplayer *self, sample_t *buffer, int num_samples, int num_channels, float scale)
   {
   int i;
   sample_t *lc, *rc, *src, gain;
   
   self->op_buffersize = num_samples * sizeof (sample_t);
   if ((!(self->leftbuffer = realloc(self->leftbuffer, self->op_buffersize))) && num_samples)
      {
      fprintf(stderr, "xlplayer: malloc failure");
      exit(5);
      }
   if ((!(self->rightbuffer = realloc(self->rightbuffer, self->op_buffersize))) && num_samples)
      {
      fprintf(stderr, "xlplayer: malloc failure");
      exit(5);
      }
   switch (num_channels)
      {
      case 0:
         break;                 /* this is a wtf case */
      case 1:
         for (lc = self->leftbuffer, src = buffer, i = 0; i < num_samples; i++)
            {
            gain = xlplayer_get_next_gain(self);        /* used for fade-in */
            *lc++ = *src++ * gain * scale;
            }
         memcpy(self->rightbuffer, self->leftbuffer, self->op_buffersize);
         break;
      case 2:
         for (lc = self->leftbuffer, rc = self->rightbuffer, src = buffer, i = 0; i < num_samples; i++)
            {
            gain = xlplayer_get_next_gain(self);
            *lc++ = *src++ * gain * scale;      /* stereo mix is a simple demultiplex job */
            *rc++ = *src++ * gain * scale;
            }
         break;
      case 3:
         for (lc = self->leftbuffer, rc = self->rightbuffer, src = buffer, i = 0; i < num_samples; i++)
            {
            gain = xlplayer_get_next_gain(self) * 0.5F;
            *lc = (*src++) * gain * scale; /* do the left and right channels */
            *rc = (*src++) * gain * scale;
            *(lc++) += (*src) *gain * scale;    /* downmix the middle channel to the left and right one */
            *(rc++) += (*src++) *gain * scale;
            }
         break;
      case 4:
         for (lc = self->leftbuffer, rc = self->rightbuffer, src = buffer, i = 0; i < num_samples; i++, src += 4)
            {
            gain = xlplayer_get_next_gain(self) * 0.5F;
            *lc++ = (src[0] + src[3]) * gain * scale;
            *rc++ = (src[2] + src[4]) * gain * scale;
            }
         break;
      case 5:
         for (lc = self->leftbuffer, rc = self->rightbuffer, src = buffer, i = 0; i < num_samples; i++, src += 5)
            {
            gain = xlplayer_get_next_gain(self) * 0.5F;
            *lc++ = (src[0] + src[3]) * gain * scale;   /* this is for 4.1 channels with sub discarded */
            *rc++ = (src[2] + src[4]) * gain * scale;
            }
         break;
      case 6:
         for (lc = self->leftbuffer, rc = self->rightbuffer, src = buffer, i = 0; i < num_samples; i++, src += 6)
            {
            gain = xlplayer_get_next_gain(self) * 0.33333333F;
            *lc++ = (src[0] + src[3] + src[4]) * gain * scale;  /* this is for 5.1 channels */
            *rc++ = (src[2] + src[4] + src[5]) * gain * scale;   /* sub discarded */
            }
         break;
      }
   }

void xlplayer_write_channel_data(struct xlplayer *self)
   {
   u_int32_t samplecount;
   
   if (self->op_buffersize > jack_ringbuffer_write_space(self->right_ch))
      {
      self->write_deferred = TRUE;      /* prevent further accumulation of data that would clobber */
      usleep(20000);
      }
   else
      {
      if (self->op_buffersize)
         {
         jack_ringbuffer_write(self->left_ch, (char *)self->leftbuffer, self->op_buffersize);
         jack_ringbuffer_write(self->right_ch, (char *)self->rightbuffer, self->op_buffersize);
         samplecount = self->op_buffersize / sizeof (sample_t);
         self->samples_written += samplecount;
         self->sleep_samples += samplecount;
         }
      self->write_deferred = FALSE;
      if (self->sleep_samples > 6000)
         {
         if (self->sleep_samples > 12000)
            usleep(20000);
         else
            usleep(10000);
         self->sleep_samples = 0;
         }
      }
   }

/* xlplayer_update_progress_time_ms: a rather ugly calculator of where the play progress is up to */
static u_int32_t xlplayer_update_progress_time_ms(struct xlplayer *self)
   {
   int32_t rb_time_ms;  /* the amount of time it would take to play all the samples in the buffer */
   int32_t progress;
   
   rb_time_ms = (float)jack_ringbuffer_read_space(self->right_ch) / sizeof (sample_t) * 1000.0f / self->samplerate;
   progress = self->samples_written * 1000.0f / self->samplerate - rb_time_ms + self->seek_s * 1000.0f;

   if (progress >= 0)
      return self->play_progress_ms = progress;
   else
      return self->play_progress_ms = 0;
   }

static char *get_extension(char *pathname)
   {
   char *p, *extension;
   
   if (!(p = strrchr(pathname, '.')))
      {
      fprintf(stderr, "get_extension: failed to find a file extension delineator '.'\n");
      return strdup("");
      }
   extension = p = strdup(p + 1);
   while (*p)
      {
      char c = tolower(*p);
      *p++ = c;
      }
   return extension;
   }

static void *xlplayer_main(struct xlplayer *self)
   {
   char *extension;
   
   for(self->up = TRUE; self->command != CMD_THREADEXIT; self->watchdog_timer = 0)
      {
      switch (self->command)
         {
         case CMD_COMPLETE:
            break;
         case CMD_PLAY:
            self->playmode = PM_INITIATE;
            break;
         case CMD_PLAYMANY:
            self->pathname = self->playlist[self->playlistindex = 0];
            self->playmode = PM_INITIATE;
            break;
         case CMD_EJECT:
            if (self->playmode != PM_STOPPED)
               self->playmode = PM_EJECTING;
            else
               {
               xlplayer_set_fadesteps(self, self->fade_mode);
               self->jack_flush = TRUE;
               while (self->jack_is_flushed == 0 && *(self->jack_shutdown_f) == FALSE)
                  usleep(10000);
               self->jack_is_flushed = 0;
               self->command = CMD_COMPLETE;
               }
            break;
         case CMD_CLEANUP:
            if (self->playlist)
               free(self->playlist);
            self->command = CMD_THREADEXIT;
         case CMD_THREADEXIT:
            continue;
         }
      switch (self->playmode)
         {
         case PM_STOPPED:
            usleep(10000);
            continue;
         case PM_INITIATE:
            self->initial_audio_context = -1;   /* pre-select failure return code */
            xlplayer_set_fadesteps(self, self->fade_mode);
            extension = get_extension(self->pathname);
            if (
                    ((!strcmp(extension, "ogg") || !strcmp(extension, "oga")) && oggdecode_reg(self))
#ifdef HAVE_SPEEX
                    || (!strcmp(extension, "spx") && oggdecode_reg(self))
#endif
#ifdef HAVE_FLAC
                    || (!strcmp(extension, "flac") && flacdecode_reg(self))
#endif
                    || ((!strcmp(extension, "wav") || !strcmp(extension, "au") || !strcmp(extension, "aiff")) && sndfiledecode_reg(self))
#ifdef HAVE_AVCODEC
#ifdef HAVE_AVFORMAT
                    || ((!strcmp(extension, "m4a") || !strcmp(extension, "mp4") || !strcmp(extension, "m4b") || !strcmp(extension, "m4p") || !strcmp(extension, "wma") || !strcmp(extension, "avi") || !strcmp(extension, "mpc") || !strcmp(extension, "ape")) && avcodecdecode_reg(self))
#endif /* HAVE_AVFORMAT */
#endif /* HAVE_AVCODEC */
                    || (!strcmp(extension, "mp3") && have_mad && mp3decode_reg(self))
               )
               {
               self->playmode = PM_PLAYING;
               self->play_progress_ms = 0;
               self->write_deferred = 0;
               self->pause = 0;
               self->samples_written = 0;
               self->sleep_samples = 0;
               self->fadein_index = (self->seek_s || self->fade_mode) ? fb_size - 1 : -1;
               self->dec_init(self);
               if (self->command != CMD_COMPLETE)
                  ++self->current_audio_context;
               self->initial_audio_context = self->current_audio_context;
               }
            else
               self->playmode = PM_STOPPED;
            self->command = CMD_COMPLETE;
            free(extension);
            break;
         case PM_PLAYING:
            if (self->write_deferred)
               xlplayer_write_channel_data(self);
            else
               self->dec_play(self);
            break;
         case PM_EJECTING:
            xlplayer_set_fadesteps(self, self->fade_mode);
            self->dec_eject(self);
            if (self->playlistmode)
               {
               if (self->command != CMD_EJECT)
                  {
                  /* implements the internal playlist here */
                  if (++self->playlistindex == self->playlistsize && self->loop)
                     self->playlistindex = 0;                   /* perform looparound if relevant */
                  if (self->playlistindex < self->playlistsize) /* check for non end of playlist */
                     {
                     self->pathname = self->playlist[self->playlistindex];
                     self->playmode = PM_INITIATE;
                     continue;
                     }
                  }
               else
                  while (self->playlistsize--)
                     free(self->playlist[self->playlistsize]);
               }
            ++self->current_audio_context;
            self->playmode = PM_STOPPED;
            break;
         } 
      }
   self->command = CMD_COMPLETE;
   return 0;
   }

/* callback functions for feeding the playback speed resampler */
static long conv_l_read(void *cb_data, float **audiodata)
   {
   struct xlplayer *self = (struct xlplayer *)cb_data;
   
   if (self->pbs_exchange == 0)         /* used to maintain mapping of input buffers after a swap */
      {
      /* try and get at least PBSPEED_INPUT_SAMPLE_SIZE samples */
      self->pbs_norm_read_qty = jack_ringbuffer_read_space(self->right_ch) / sizeof (sample_t);
      if (self->pbs_norm_read_qty > PBSPEED_INPUT_SAMPLE_SIZE)
         self->pbs_norm_read_qty = PBSPEED_INPUT_SAMPLE_SIZE;
      
      jack_ringbuffer_read(self->left_ch, (char *)self->pbsrb_l, self->pbs_norm_read_qty * sizeof (sample_t));
      *audiodata = self->pbsrb_l;
      return self->pbs_norm_read_qty;
      }
   else
      {
      self->pbs_fade_read_qty = jack_ringbuffer_read_space(self->left_fade) / sizeof (sample_t);
      if (self->pbs_fade_read_qty > PBSPEED_INPUT_SAMPLE_SIZE)
         self->pbs_fade_read_qty = PBSPEED_INPUT_SAMPLE_SIZE;
      
      jack_ringbuffer_read(self->left_fade, (char *)self->pbsrb_lf, self->pbs_fade_read_qty * sizeof (sample_t));
      *audiodata = self->pbsrb_lf;
      return self->pbs_fade_read_qty;
      }
   }

static long conv_r_read(void *cb_data, float **audiodata)
   {
   struct xlplayer *self = (struct xlplayer *)cb_data;
   
   if (self->pbs_exchange == 0)
      {
      jack_ringbuffer_read(self->right_ch, (char *)self->pbsrb_r, self->pbs_norm_read_qty * sizeof (sample_t));
      *audiodata = self->pbsrb_r;
      return self->pbs_norm_read_qty;
      }
   else
      {
      jack_ringbuffer_read(self->right_fade, (char *)self->pbsrb_rf, self->pbs_fade_read_qty * sizeof (sample_t));
      *audiodata = self->pbsrb_rf;
      return self->pbs_fade_read_qty;
      }
   }

static long conv_lf_read(void *cb_data, float **audiodata)
   {
   struct xlplayer *self = (struct xlplayer *)cb_data;
   
   if (self->pbs_exchange == 0)
      {
      self->pbs_fade_read_qty = jack_ringbuffer_read_space(self->left_fade) / sizeof (sample_t);
      if (self->pbs_fade_read_qty > PBSPEED_INPUT_SAMPLE_SIZE)
         self->pbs_fade_read_qty = PBSPEED_INPUT_SAMPLE_SIZE;
      
      jack_ringbuffer_read(self->left_fade, (char *)self->pbsrb_lf, self->pbs_fade_read_qty * sizeof (sample_t));
      *audiodata = self->pbsrb_lf;
      return self->pbs_fade_read_qty;
      }
   else
      {
      self->pbs_norm_read_qty = jack_ringbuffer_read_space(self->right_ch) / sizeof (sample_t);
      if (self->pbs_norm_read_qty > PBSPEED_INPUT_SAMPLE_SIZE)
         self->pbs_norm_read_qty = PBSPEED_INPUT_SAMPLE_SIZE;
      
      jack_ringbuffer_read(self->left_ch, (char *)self->pbsrb_l, self->pbs_norm_read_qty * sizeof (sample_t));
      *audiodata = self->pbsrb_l;
      return self->pbs_norm_read_qty;
      }
   }
static long conv_rf_read(void *cb_data, float **audiodata)
   {
   struct xlplayer *self = (struct xlplayer *)cb_data;
   
   if (self->pbs_exchange == 0)
      {
      jack_ringbuffer_read(self->right_fade, (char *)self->pbsrb_rf, self->pbs_fade_read_qty * sizeof (sample_t));
      *audiodata = self->pbsrb_rf;
      return self->pbs_fade_read_qty;
      }
   else
      {
      jack_ringbuffer_read(self->right_ch, (char *)self->pbsrb_r, self->pbs_norm_read_qty * sizeof (sample_t));
      *audiodata = self->pbsrb_r;
      return self->pbs_norm_read_qty;
      }
   }

struct xlplayer *xlplayer_create(int samplerate, double duration, char *playername, int *shutdown_f)
   {
   struct xlplayer *self;
   int error;
   
   if (!(self = malloc(sizeof (struct xlplayer))))
      {
      fprintf(stderr, "xlplayer: malloc failure");
      exit(5);
      }
   self->rbsize = (int)(duration * samplerate) << 2;
   self->rbdelay = (int)(duration * 1000);
   if (!(self->left_ch = jack_ringbuffer_create(self->rbsize)))
      {
      fprintf(stderr, "xlplayer: ringbuffer creation failure");
      exit(5);
      }
   if (!(self->right_ch = jack_ringbuffer_create(self->rbsize)))
      {
      fprintf(stderr, "xlplayer: ringbuffer creation failure");
      exit(5);
      }
   if (!(self->left_fade = jack_ringbuffer_create(self->rbsize)))
      {
      fprintf(stderr, "xlplayer: ringbuffer creation failure");
      exit(5);
      }
   if (!(self->right_fade = jack_ringbuffer_create(self->rbsize)))
      {
      fprintf(stderr, "xlplayer: ringbuffer creation failure");
      exit(5);
      }
   if (!(self->pbspeed_conv_l = src_callback_new(conv_l_read, SRC_LINEAR, 1, &error, self)))
      {
      fprintf(stderr, "xlplayer: playback speed converter initialisation failure");
      exit(5);
      }
   if (!(self->pbspeed_conv_r = src_callback_new(conv_r_read, SRC_LINEAR, 1, &error, self)))
      {
      fprintf(stderr, "xlplayer: playback speed converter initialisation failure");
      exit(5);
      }
   if (!(self->pbspeed_conv_lf = src_callback_new(conv_lf_read, SRC_LINEAR, 1, &error, self)))
      {
      fprintf(stderr, "xlplayer: playback speed converter initialisation failure");
      exit(5);
      }
   if (!(self->pbspeed_conv_rf = src_callback_new(conv_rf_read, SRC_LINEAR, 1, &error, self)))
      {
      fprintf(stderr, "xlplayer: playback speed converter initialisation failure");
      exit(5);
      }
   if (pthread_mutex_init(&(self->dynamic_metadata.meta_mutex), NULL))
      {
      fprintf(stderr, "xlplayer: failed initialising metadata_mutex\n");
      exit(5);
      }
   self->pbsrb_l = malloc(PBSPEED_INPUT_BUFFER_SIZE);
   self->pbsrb_r = malloc(PBSPEED_INPUT_BUFFER_SIZE);
   self->pbsrb_lf = malloc(PBSPEED_INPUT_BUFFER_SIZE);
   self->pbsrb_rf = malloc(PBSPEED_INPUT_BUFFER_SIZE);
   if (!(self->pbsrb_l && self->pbsrb_r && self->pbsrb_lf && self->pbsrb_rf))
      {
      fprintf(stderr, "xlplayer: playback speed converter input buffer initialisation failure\n");
      exit(5);
      }
   self->playername = playername;
   self->leftbuffer = self->rightbuffer = NULL;
   self->have_data_f = self->have_swapped_buffers_f = 0;
   self->pause = 0;
   self->jack_flush = self->jack_is_flushed = 0;
   self->fadeindex = -1;
   self->fade_mode = 0;
   self->fadeout_f = 0;
   self->fadein_index = -1;
   self->dither = 0;
   self->seed = 17234;
   self->samplerate = samplerate;
   self->current_audio_context = 0;
   self->playlist = NULL;
   self->noflush = FALSE;
   self->jack_shutdown_f = shutdown_f;
   self->watchdog_timer = 0;
   self->command = CMD_COMPLETE;
   self->playmode = PM_STOPPED;
   self->up = FALSE;
   self->pbspeed = 1.0F;
   self->pbs_exchange = 0;
   self->samples_written = 0;
   self->seek_s = 0;
   self->play_progress_ms = 0;
   self->dynamic_metadata.data_type = DM_NONE_NEW;
   self->dynamic_metadata.artist = NULL;
   self->dynamic_metadata.title = NULL;
   self->dynamic_metadata.artist_title = NULL;
   self->dynamic_metadata.current_audio_context = 0;
   self->dynamic_metadata.rbdelay = 0;
   pthread_create(&self->thread, NULL, (void *(*)(void *)) xlplayer_main, self);
   while (self->up == FALSE)
      usleep(10000);
   return self;
   }

void xlplayer_destroy(struct xlplayer *self)
   {
   if (self)
      {
      self->command = CMD_CLEANUP;
      pthread_join(self->thread, NULL);
      pthread_mutex_destroy(&(self->dynamic_metadata.meta_mutex));
      free(self->pbsrb_l);
      free(self->pbsrb_r);
      free(self->pbsrb_lf);
      free(self->pbsrb_rf);
      src_delete(self->pbspeed_conv_l);
      src_delete(self->pbspeed_conv_r);
      src_delete(self->pbspeed_conv_lf);
      src_delete(self->pbspeed_conv_rf);
      jack_ringbuffer_free(self->left_ch);
      jack_ringbuffer_free(self->right_ch);
      jack_ringbuffer_free(self->left_fade);
      jack_ringbuffer_free(self->right_fade);
      free(self);
      }
   }

int xlplayer_play(struct xlplayer *self, char *pathname, int seek_s, int size, float gain_db)
   {
   xlplayer_eject(self);
   self->pathname = pathname;
   self->gain = pow(10.0, gain_db / 20.0);
   self->seek_s = seek_s;
   self->size = size;
   self->loop = FALSE;
   self->usedelay = FALSE;
   self->playlistmode = FALSE;
   self->command = CMD_PLAY;
   while (self->command)
      usleep(10000);
   return self->initial_audio_context;
   }

int xlplayer_playmany(struct xlplayer *self, char *playlist, int loop_f)
   {
   char *start = playlist, *end;
   int payloadlen, i;

   xlplayer_eject(self);
   /* this is where we parse the playlist starting with getting the number of entries */
   while (*start++ != '#');
   start[-1] = '\0';
   self->playlistsize = atoi(playlist);
   /* generate an array of pointers to point to the playlist entries which must be a copy */
   if (!(self->playlist = realloc(self->playlist, self->playlistsize * sizeof (char *))))
      {
      fprintf(stderr, "xlplayer: malloc failure\n");
      exit(5);
      }
   /* now we parse the playlist entries */
   for (i = 0; *start++ == 'd'; i++)
      {
      for (end = start; *end != ':'; end++);
      *end = '\0';
      payloadlen = atoi(start);
      start = end + 1;
      end = start + payloadlen;
      if ((self->playlist[i] = malloc(payloadlen + 1)))
         {
         memcpy(self->playlist[i], start, payloadlen);
         self->playlist[i][payloadlen] = '\0';
         }
      else
         {
         fprintf(stderr, "xlplayer: malloc failure\n");
         exit(5);
         }
      start = end;
      }
   self->gain = 1.0;
   self->seek_s = 0;
   self->loop = loop_f;
   self->playlistmode = TRUE;
   self->command = CMD_PLAYMANY;
   while (self->command)
      usleep(10000);
   return self->initial_audio_context;
   }

int xlplayer_play_noflush(struct xlplayer *self, char *pathname, int seek_s, int size, float gain_db)
   {
   self->noflush = TRUE;
   xlplayer_eject(self);
   self->pathname = pathname;
   self->gain = pow(10.0, gain_db / 20.0);
   self->seek_s = seek_s;
   self->size = size;
   self->loop = FALSE;
   self->playlistmode = FALSE;
   self->command = CMD_PLAY;
   while (self->command)
      usleep(10000);
   self->noflush = FALSE;
   return self->initial_audio_context;
   }
   
void xlplayer_pause(struct xlplayer *self)
   {
   self->pause = TRUE;
   }
   
void xlplayer_unpause(struct xlplayer *self)
   {
   self->pause = FALSE;
   }

void xlplayer_dither(struct xlplayer *self, int dither_f)
   {
   self->dither = dither_f;
   }
 
void xlplayer_eject(struct xlplayer *self)
   {
   if (!self->fadeout_f)
      xlplayer_pause(self);
   self->command = CMD_EJECT;
   while (self->command)
      usleep(10000);
   }

void xlplayer_set_fadesteps(struct xlplayer *self, int fade_mode)
   {
   static int a[] = { 10, 2, 1 };
   static int b[] = { 200, 2, 1 };

   self->fadeoutstep = a[fade_mode];
   self->fadeinstep = b[fade_mode];
   }

/* version supporting playback speed variance */
size_t read_from_player_sv(struct xlplayer *self, sample_t *left_buf, sample_t *right_buf, sample_t *left_fbuf, sample_t *right_fbuf, jack_nframes_t nframes)
   {
   jack_ringbuffer_t *swap;
   SRC_STATE *pbs_swap;
   float *pbsrb_swap;
   size_t todo = 0, ftodo = 0;

   self->have_swapped_buffers_f = FALSE;
   if (self->jack_flush)
      {
      if (self->noflush == FALSE)
         {
         if (self->pause == 0)
            {
            /* perform the exchange of handles for the purpose of fading out the remaining buffer contents */
            /* exchange speed converter handles */
            pbs_swap = self->pbspeed_conv_l;
            self->pbspeed_conv_l = self->pbspeed_conv_lf;
            self->pbspeed_conv_lf = pbs_swap;
            pbs_swap = self->pbspeed_conv_r;
            self->pbspeed_conv_r = self->pbspeed_conv_rf;
            self->pbspeed_conv_rf = pbs_swap;
            /* exchange speed converter input buffers */
            pbsrb_swap = self->pbsrb_l;
            self->pbsrb_l = self->pbsrb_lf;
            self->pbsrb_lf = pbsrb_swap;
            pbsrb_swap = self->pbsrb_r;
            self->pbsrb_r = self->pbsrb_rf;
            self->pbsrb_rf = pbsrb_swap;
            self->pbs_exchange = !self->pbs_exchange;
            /* exchange ring buffers */
            swap = self->left_ch;
            self->left_ch = self->left_fade;
            self->left_fade = swap;
            swap = self->right_ch;
            self->right_ch = self->right_fade;
            self->right_fade = swap;
            /* initialisations for fade */
            self->fadeindex = 0;
            self->have_swapped_buffers_f = TRUE;
            }
         /* buffer flushing */
         src_reset(self->pbspeed_conv_l);
         src_reset(self->pbspeed_conv_r);
         jack_ringbuffer_reset(self->left_ch);
         jack_ringbuffer_reset(self->right_ch);
         }
      self->jack_is_flushed = 1;
      self->jack_flush = 0;
      self->pause = 0;
      }
   
   if (self->pause == 0)
      {
      if (self->pbspeed != self->newpbspeed)
         {
         self->pbspeed = self->newpbspeed;
         src_set_ratio(self->pbspeed_conv_l, self->pbspeed);    /* bug workaround for libsamplerate 0.1.2 */
         src_set_ratio(self->pbspeed_conv_r, self->pbspeed);
         src_set_ratio(self->pbspeed_conv_lf, self->pbspeed);
         src_set_ratio(self->pbspeed_conv_rf, self->pbspeed);
         }
      /* the number of samples in the ring buffer used when calculating play progress */
      /* samples stored in the resampler are not worth the bother of accounting for */
      self->avail = jack_ringbuffer_read_space(self->right_ch) / sizeof (sample_t);
      /* read data from playback speed resampler */
      todo = src_callback_read(self->pbspeed_conv_l, self->pbspeed, nframes, left_buf);
      src_callback_read(self->pbspeed_conv_r, self->pbspeed, todo, right_buf);
      memset(left_buf + self->avail, 0, (nframes - todo) * sizeof (sample_t));
      memset(right_buf + self->avail, 0, (nframes - todo) * sizeof (sample_t));
      /* read fade data from playback speed resampler */
      if (left_fbuf && right_fbuf)
         {
         ftodo = src_callback_read(self->pbspeed_conv_lf, self->pbspeed, nframes, left_fbuf);
         src_callback_read(self->pbspeed_conv_rf, self->pbspeed, ftodo, right_fbuf);
         memset(left_fbuf + ftodo, 0, (nframes - ftodo) * sizeof (sample_t));
         memset(right_fbuf + ftodo, 0, (nframes - ftodo) * sizeof (sample_t));
         }
      self->have_data_f = todo > 0;
      }
   else
      {
      memset(left_buf, 0, nframes * sizeof (sample_t));
      memset(right_buf, 0, nframes * sizeof (sample_t));
      if (left_fbuf && right_fbuf)
         {
         memset(left_fbuf, 0, nframes * sizeof (sample_t));
         memset(right_fbuf, 0, nframes * sizeof (sample_t));
         }
      }
   xlplayer_update_progress_time_ms(self);
   return todo;
   }

/* version not supporting playback speed variance but uses less CPU */
size_t read_from_player(struct xlplayer *self, sample_t *left_buf, sample_t *right_buf, sample_t *left_fbuf, sample_t *right_fbuf, jack_nframes_t nframes)
   {
   jack_ringbuffer_t *swap;
   size_t todo, favail, ftodo;
   
   self->have_swapped_buffers_f = FALSE;
   if (self->jack_flush)
      {
      if (self->noflush == FALSE)
         {
         if (self->pause == 0)
            {
            swap = self->left_ch;
            self->left_ch = self->left_fade;
            self->left_fade = swap;
            swap = self->right_ch;
            self->right_ch = self->right_fade;
            self->right_fade = swap;
            self->fadeindex = 0;
            self->have_swapped_buffers_f = TRUE;
            }
         jack_ringbuffer_reset(self->left_ch);
         jack_ringbuffer_reset(self->right_ch);
         }
      self->jack_is_flushed = 1;
      self->jack_flush = 0;
      self->pause = 0;
      }
   
   self->avail = jack_ringbuffer_read_space(self->right_ch) / sizeof (sample_t);
   todo = (self->avail > nframes ? nframes : self->avail);
   favail = jack_ringbuffer_read_space(self->right_fade) / sizeof (sample_t);
   ftodo = (favail > nframes ? nframes : favail);
   
   if (self->pause == 0)
      {
      /* fill the frame with whatever data is available, then pad as needed with zeroes */
      jack_ringbuffer_read(self->left_ch, (char *)left_buf, todo * sizeof (sample_t));
      memset(left_buf + todo, 0, (nframes - todo) * sizeof (sample_t)); 
      jack_ringbuffer_read(self->right_ch, (char *)right_buf, todo * sizeof (sample_t));
      memset(right_buf + todo, 0, (nframes - todo) * sizeof (sample_t));
      if (left_fbuf && right_fbuf)
         {
         jack_ringbuffer_read(self->left_fade, (char *)left_fbuf, ftodo * sizeof (sample_t));
         memset(left_fbuf + ftodo, 0, (nframes - ftodo) * sizeof (sample_t)); 
         jack_ringbuffer_read(self->right_fade, (char *)right_fbuf, ftodo * sizeof (sample_t));
         memset(right_fbuf + ftodo, 0, (nframes - ftodo) * sizeof (sample_t));
         }
      self->have_data_f = todo > 0;
      }
   else
      {
      memset(left_buf, 0, nframes * sizeof (sample_t));
      memset(right_buf, 0, nframes * sizeof (sample_t));
      if (left_fbuf && right_fbuf)
         {
         memset(left_fbuf, 0, nframes * sizeof (sample_t));
         memset(right_fbuf, 0, nframes * sizeof (sample_t));
         }
      }
   xlplayer_update_progress_time_ms(self);
   return todo;
   }

int xlplayer_calc_rbdelay(struct xlplayer *xlplayer)
   {
   return jack_ringbuffer_read_space(xlplayer->left_ch) * 1000 / (sizeof (sample_t) * xlplayer->samplerate);
   }

void xlplayer_set_dynamic_metadata(struct xlplayer *xlplayer, enum metadata_t type, char *artist, char *title, char *artist_title, int delay)
   {
   struct xlp_dynamic_metadata *dm = &(xlplayer->dynamic_metadata);
   
   pthread_mutex_lock(&(dm->meta_mutex));
   dm->data_type = type;
   if (dm->artist)
      free(dm->artist);
   if (dm->title)
      free(dm->title);
   if (dm->artist_title)
      free(dm->artist_title);
   dm->artist = strdup(artist);
   dm->title = strdup(title);
   dm->artist_title = strdup(artist_title);
   dm->current_audio_context = xlplayer->current_audio_context;
   dm->rbdelay = delay;
   pthread_mutex_unlock(&(dm->meta_mutex));
   }