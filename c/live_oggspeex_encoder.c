/*
#   live_oggspeex_encoder.c: encode speex from a live source into an ogg container
#   Copyright (C) 2008 Stephen Fairchild (s-fairchild@users.sourceforge.net)
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

#include "live_oggspeex_encoder.h"
#include "live_ogg_encoder.h"

#ifdef HAVE_SPEEX

#include <stdio.h>
#include <string.h>

#define TRUE 1
#define FALSE 0
#define SUCCEEDED 1
#define FAILED 0

#define MAX_FRAME_BYTES 2000

#define readint(buf, base) (((buf[base+3]<<24)&0xff000000)| \
                           ((buf[base+2]<<16)&0xff0000)| \
                           ((buf[base+1]<<8)&0xff00)| \
                            (buf[base]&0xff))
#define writeint(buf, base, val) do{ buf[base+3]=((val)>>24)&0xff; \
                                     buf[base+2]=((val)>>16)&0xff; \
                                     buf[base+1]=((val)>>8)&0xff; \
                                     buf[base]=(val)&0xff; \
                                 }while(0)

#ifndef HAVE_SPEEX_HEADER_FREE
#define speex_header_free(h) speex_free(h)
#endif


static void live_oggspeex_build_metadata(struct encoder *encoder, struct lose_data *s)
   {
   char *r, *w, *e, *marker;    /* read, write, end, the placemarker string */
   int count;   /* the number of occurrences of marker in metaformat */
   int len;
   size_t size;
   int items = 0;
   int base;

   if (s->artist)
      free(s->artist);
   if (s->title)
      free(s->title);
   pthread_mutex_lock(&encoder->metadata_mutex);
   for (count = 0, r = encoder->metaformat; (r = strstr(r, "%s")); count++, r += 2);
   if (count == 0)
      {
      /* handle metadata does not contain artist - title */
      s->artist = NULL;
      if (encoder->metaformat && encoder->metaformat[0] != '\0')
         {
         if (!(s->title = malloc(strlen(encoder->metaformat) + 7)))
            {
            fprintf(stderr, "live_oggspeex_build_metadata: malloc failure\n");
            return;
            }
         sprintf(s->title, "TITLE=%s", encoder->metaformat);
         }
      else
         s->title = NULL;
      }
   else
      {
      /* handle metadata only contains artist - title */
      if (count == 1 && !strcmp(encoder->metaformat, "%s"))
         {
         if (encoder->artist && encoder->artist[0] != '\0')
            {
            if (!(s->artist = malloc(strlen(encoder->artist) + 8)))
               {
               fprintf(stderr, "live_oggspeex_build_metadata: malloc failure\n");
               return;
               }
            sprintf(s->artist, "author=%s", encoder->artist);
            }
         else
            s->artist = NULL;
         if (encoder->title && encoder->title[0] != '\0')
            {
            if (!(s->title = malloc(strlen(encoder->title) + 7)))
               {
               fprintf(stderr, "live_oggspeex_build_metadata: malloc failure\n");
               return;
               }
            sprintf(s->title, "title=%s", encoder->title);
            }
         else
            s->title = NULL;
         }
      else
         {
         /* handle a mix of possible multiple "artist - title" and or other text */
         /* in python: artist = metaformat.replace("%s", artist_title) */
         /* in C see below LOL */
         s->title = malloc(size = count * strlen(encoder->artist_title) + strlen(encoder->metaformat) - count * strlen(marker = "%s") + 7);
         if (!s->title)
            fprintf(stderr, "live_oggspeex_build_metadata: malloc failure\n");
         else
            {
            strcpy(s->title, "TITLE=");
            len = strlen(encoder->artist_title);
            r = encoder->metaformat;
            w = s->title + 6;
            for (;;)
               {
               if ((e = strstr(r, marker)))
                  {
                  memcpy(w, r, e - r);  /* copy the text before the %s */
                  w += e - r;           /* advance write pointer */
                  memcpy(w, encoder->artist_title, len); /* copy artist - title */
                  w += len;             /* advance the write pointer */
                  r = e + 2;            /* skip over the %s */
                  }
               else
                  {
                  strcpy(w, r); /* copy the remaining text and null terminate */
                  break;        /* finished */
                  }
               }
            }
         s->artist = NULL;
         }
      }
   
   s->metadata_vclen = 8 + s->vs_len;
   if (s->artist)
      {
      items++;
      s->metadata_vclen += 4 + strlen(s->artist);
      }
      
   if (s->title)
      {
      items++;
      s->metadata_vclen += 4 + strlen(s->title);
      }
      
   if (!(s->metadata_vc = realloc(s->metadata_vc, s->metadata_vclen)))
      {
      fprintf(stderr, "live_oggspeex_build_metadata: malloc failure\n");
      s->metadata_vclen = 0;
      return;
      }
      
   writeint(s->metadata_vc, 0, s->vs_len);
   memcpy(s->metadata_vc + 4, s->vendor_string, s->vs_len);
   writeint(s->metadata_vc, 4 + s->vs_len, items);
   base = 8 + s->vs_len;
   
   if (s->artist)
      {
      writeint(s->metadata_vc, base, (len = strlen(s->artist)));
      memcpy(s->metadata_vc + base + 4, s->artist, len);
      base += 4 + len;
      }
   
   if (s->title)
      {
      writeint(s->metadata_vc, base, (len = strlen(s->title)));
      memcpy(s->metadata_vc + base + 4, s->title, len);
      base += 4 + len;
      }
   
   if (base != s->metadata_vclen)
      fprintf(stderr, "live_oggspeex_build_metadata: incorrect size assumption }}}}}}\n");
   else
      fprintf(stderr, "vorbis comment created successfully\n");
   
   
   pthread_mutex_unlock(&encoder->metadata_mutex);
   fprintf(stderr, "live_oggspeex_build_metadata: metadata for encoder %d\n%s\n%s\n", encoder->numeric_id, s->artist, s->title);
   }

static void live_oggspeex_encoder_monomix(float *in, float *out, size_t n)
   {
   while (n--)
      *out++ = *in++ * 32768;
   }
   
static void live_oggspeex_encoder_stereomix(float *l, float *r, float *m, size_t n)
   {
   while (n--)
      {
      *m++ = *l++ * 32768;
      *m++ = *r++ * 32768;
      }
   }

static void live_oggspeex_encoder_main(struct encoder *encoder)
   {
   struct lose_data * const s = encoder->encoder_private;
   ogg_page og;
   ogg_packet op;
   
   if (encoder->encoder_state == ES_STARTING)
      {
      SpeexHeader header;
      struct SpeexMode const *mode;
      struct SpeexMode const *modelist[] = { &speex_uwb_mode, &speex_wb_mode, &speex_nb_mode };
      int ratelist[] = { 32000, 16000, 8000 };
      char *packet;
      int packet_size;
      
      speex_bits_init(&s->bits);
      if (!(s->enc_state = speex_encoder_init(mode = modelist[s->mode])))
         {
         fprintf(stderr, "live_oggspeex_encoder_main: failed to initialise speex encoder\n");
         goto bailout;
         }
         
      speex_encoder_ctl(s->enc_state, SPEEX_GET_FRAME_SIZE, &s->fsamples);
      speex_encoder_ctl(s->enc_state, SPEEX_SET_QUALITY, &s->quality);
      speex_encoder_ctl(s->enc_state, SPEEX_SET_COMPLEXITY, &s->complexity);
      speex_encoder_ctl(s->enc_state, SPEEX_GET_LOOKAHEAD, &s->lookahead);
      
      if (!(s->inbuf = realloc(s->inbuf, s->fsamples * encoder->n_channels * sizeof (float))))
         {
         fprintf(stderr, "live_oggspeex_encoder_main: malloc failure\n");
         goto bailout;
         }
      
      speex_init_header(&header, ratelist[s->mode], encoder->n_channels, mode);
      header.frames_per_packet = 1;
      if (!(packet = speex_header_to_packet(&header, &packet_size)))
         {
         fprintf(stderr, "live_oggspeex_encoder_main: failed to make header packet\n");
         goto bailout;
         }
         
      ogg_stream_init(&s->os, ++encoder->oggserial);
      op.packet = (unsigned char *)packet;
      op.bytes = packet_size;
      op.b_o_s = 1;
      op.e_o_s = 0;
      op.granulepos = 0;
      op.packetno = 0;
      ogg_stream_packetin(&s->os, &op);
      speex_header_free(packet);
      s->pflags = PF_INITIAL | PF_OGG | PF_HEADER;
      
      while (ogg_stream_flush(&s->os, &og))
         {
         if (!(live_ogg_write_packet(encoder, &og, s->pflags)))
            {
            fprintf(stderr, "live_ogg_write_packet: failed to write header\n");
            goto bailout;
            }
         s->pflags = PF_OGG | PF_HEADER;
         }

      if (encoder->new_metadata)
         {
         encoder->new_metadata = FALSE;
         
         if (s->use_metadata)
            live_oggspeex_build_metadata(encoder, s);
         else
            {
            /* make a bare-bones vorbis comment block */
            fprintf(stderr, "making bare-bones comment block\n");
            if (!(s->metadata_vc = realloc(s->metadata_vc, s->metadata_vclen = s->vs_len + 8)))
               {
               fprintf(stderr, "live_ogg_write_packet: malloc failure\n");
               goto bailout;
               }
            
            writeint(s->metadata_vc, 0, s->vs_len);
            memcpy(s->metadata_vc + 4, s->vendor_string, s->vs_len);
            writeint(s->metadata_vc, 4 + s->vs_len, 0);
            }
         }
         
      op.packet = (unsigned char *)s->metadata_vc;
      op.bytes = s->metadata_vclen;
      op.b_o_s = 0;
      op.e_o_s = 0;
      op.granulepos = 0;
      op.packetno = 1;
      ogg_stream_packetin(&s->os, &op);
      
      while (ogg_stream_flush(&s->os, &og))
         {
         if (!(live_ogg_write_packet(encoder, &og, s->pflags)))
            {
            fprintf(stderr, "live_ogg_write_packet: failed to write header\n");
            goto bailout;
            }
         }
      
      s->pflags = PF_OGG;
      s->packetno = 2;
      s->frame = 0;
      s->total_samples = 0;
      s->samples_encoded = -s->lookahead;
      s->eos = FALSE;
      encoder->timestamp = 0.0;
      encoder->encoder_state = ES_RUNNING;
      return;
      }
      
   if (encoder->encoder_state == ES_RUNNING)
      {
      struct encoder_ip_data *id;
      char wb[MAX_FRAME_BYTES];
      int ws;
      int (*ogg_paging_function)(ogg_stream_state *, ogg_page *);
 
      if (!(encoder->watchdog_info.tick & 127))
         fprintf(stderr, "encoder %d running\n", encoder->numeric_id);  
         
      if (s->eos == FALSE)
         {
         if ((encoder->new_metadata && s->use_metadata) || !encoder->run_request_f || encoder->flush)
            {
            s->eos = TRUE;
            memset(s->inbuf, '\0', s->fsamples * encoder->n_channels * sizeof (float));
            return;
            }
         else
            {
            if((id = encoder_get_input_data(encoder, s->fsamples, s->fsamples, NULL)))
               {
               if (encoder->n_channels == 2)
                  {
                  live_oggspeex_encoder_stereomix(id->buffer[0], id->buffer[1], s->inbuf, s->fsamples);
                  speex_encode_stereo(s->inbuf, s->fsamples, &s->bits);
                  }
               else
                  live_oggspeex_encoder_monomix(id->buffer[0], s->inbuf, s->fsamples);
               
               encoder_ip_data_free(id);
               s->total_samples += s->fsamples;
               }
            else
               return;     /* no new audio data available */
            }
         }
      else
         if (encoder->n_channels == 2)
            speex_encode_stereo(s->inbuf, s->fsamples, &s->bits);
       
      speex_encode(s->enc_state, s->inbuf, &s->bits);
      speex_bits_insert_terminator(&s->bits);
      ws = speex_bits_write(&s->bits, wb, MAX_FRAME_BYTES);
      speex_bits_reset(&s->bits);
      s->samples_encoded += s->fsamples;
 
      op.packet = (unsigned char *)wb;
      op.bytes = ws;
      op.b_o_s = 0;
      op.packetno = s->packetno++;
      if (s->samples_encoded >= s->total_samples)
         {
         op.e_o_s = 1;
         op.granulepos = s->total_samples;
         }
      else
         {
         op.e_o_s = 0;
         op.granulepos = s->samples_encoded;
         }
 
      if (op.e_o_s || ++s->frame == 10)
         ogg_paging_function = ogg_stream_flush;
      else
         ogg_paging_function = ogg_stream_pageout;

      ogg_stream_packetin(&s->os, &op);

      while (ogg_paging_function(&s->os, &og))
         {
         s->frame = 0;
         if (ogg_page_eos(&og))
            {
            s->pflags |= PF_FINAL;
            encoder->flush = FALSE;
            encoder->encoder_state = ES_STOPPING;
            }
         if (!live_ogg_write_packet(encoder, &og, s->pflags))
            {
            fprintf(stderr, "live_oggspeex_encoder_main: failed to write packet\n");
            goto bailout;
            }
         }
      return;
      }

   if (encoder->encoder_state == ES_STOPPING)
      {
      speex_bits_destroy(&s->bits);
      speex_encoder_destroy(s->enc_state);
      s->enc_state = NULL;
      ogg_stream_clear(&s->os);
      if (!encoder->run_request_f)
         goto bailout;
      else
         encoder->encoder_state = ES_STARTING;
      return;
      }
      
   fprintf(stderr, "live_oggspeex_encoder_main: unhandled encoder state\n");
   return;
   
   bailout:
   fprintf(stderr, "live_oggspeex_encoder_main: performing cleanup\n");
   encoder->run_request_f = FALSE;
   encoder->encoder_state = ES_STOPPED;
   encoder->run_encoder = NULL;
   encoder->flush = FALSE;
   encoder->new_metadata = FALSE;
   encoder->encoder_private = NULL;
   if (s->enc_state)
      {
      speex_bits_destroy(&s->bits);
      speex_encoder_destroy(s->enc_state);
      }
      
   if (s->inbuf)
      free(s->inbuf);
   if (s->metadata_vc)
      free(s->metadata_vc);
   if (s->artist)
      free(s->artist);
   if (s->title)
      free(s->title);
   free(s);
   fprintf(stderr, "live_oggspeex_encoder_main: finished cleanup\n");
   return;
   }

int live_oggspeex_encoder_init(struct encoder *encoder, struct encoder_vars *ev)
   {
   struct lose_data * const s = calloc(1, sizeof (struct lose_data));
   char *speex_version;

   if (!s)
      {
      fprintf(stderr, "live_oggspeex_encoder: malloc failure\n");
      return FAILED;
      }

   speex_lib_ctl(SPEEX_LIB_GET_VERSION_STRING, (void *)&speex_version);
   snprintf(s->vendor_string, sizeof(s->vendor_string), "Encoded with Speex %s", speex_version); 
   s->vs_len = strlen(s->vendor_string);

   s->mode = atoi(ev->speex_mode);
   s->quality = atoi(ev->speex_quality);
   s->complexity = atoi(ev->speex_complexity);
   s->use_metadata = atoi(ev->use_metadata);
   encoder->samplerate = atoi(ev->sample_rate);
   encoder->encoder_private = s;
   encoder->run_encoder = live_oggspeex_encoder_main;
   return SUCCEEDED;
   }

#endif