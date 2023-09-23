/* -*- C++ -*-
 *
 *  ONScripterLabel_sound.cpp - Methods for playing sound for ONScripter-EN
 *
 *  Copyright (c) 2001-2011 Ogapee. All rights reserved.
 *  (original ONScripter, of which this is a fork).
 *
 *  ogapee@aqua.dti2.ne.jp
 *
 *  Copyright (c) 2008-2011 "Uncle" Mion Sonozaki
 *
 *  UncleMion@gmail.com
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program; if not, see <http://www.gnu.org/licenses/>
 *  or write to the Free Software Foundation, Inc.,
 *  59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 */

// Modified by Haeleth, Autumn 2006, to better support OS X/Linux packaging.

// Modified by Mion, April 2009, to update from
// Ogapee's 20090331 release source code.

// Modified by Mion, November 2009, to update from
// Ogapee's 20091115 release source code.

#include "ONScripterLabel.h"
#include <new>
#ifdef LINUX
#include <signal.h>
#endif

#ifdef USE_AVIFILE
//#include "AVIWrapper.h"
#include "FFMpegWrapper.h"
#endif


extern "C" void waveCallback(int channel);

struct WAVE_HEADER{
    char chunk_riff[4];
    char riff_length[4];
    // format chunk
    char fmt_id[8];
    char fmt_size[4];
    char data_fmt[2];
    char channels[2];
    char frequency[4];
    char byte_size[4];
    char sample_byte_size[2];
    char sample_bit_size[2];
} header;
struct WAVE_DATA_HEADER{
    // data chunk
    char chunk_id[4];
    char data_length[4];
} data_header;
static void setupWaveHeader( unsigned char *buffer, int channels, int bits,
                             unsigned long rate, unsigned long data_length,
                             unsigned int extra_bytes=0, unsigned char *extra_ptr=NULL );

extern bool ext_music_play_once_flag;

extern "C"{
    extern void mp3callback( void *userdata, Uint8 *stream, int len );
    extern void oggcallback( void *userdata, Uint8 *stream, int len );
    extern Uint32 SDLCALL cdaudioCallback( Uint32 interval, void *param );
    extern Uint32 SDLCALL silentmovieCallback( Uint32 interval, void *param );
#if defined(MACOSX) //insani
    extern Uint32 SDLCALL seqmusicSDLCallback( Uint32 interval, void *param );
#endif
}
extern void seqmusicCallback( int sig );
extern void musicCallback( int sig );
extern SDL_TimerID timer_cdaudio_id;
extern SDL_TimerID timer_silentmovie_id;

#if defined(MACOSX) //insani
extern SDL_TimerID timer_seqmusic_id;
#endif

#define TMP_SEQMUSIC_FILE "tmp.mus"
#define TMP_MUSIC_FILE "tmp.mus"

#define SWAP_SHORT_BYTES(sptr){          \
            Uint8 *bptr = (Uint8 *)sptr; \
            Uint8 tmpb = *bptr;          \
            *bptr = *(bptr+1);           \
            *(bptr+1) = tmpb;            \
        }

//WMA header format
#define IS_ASF_HDR(buf)                           \
         ((buf[0] == 0x30) && (buf[1] == 0x26) && \
          (buf[2] == 0xb2) && (buf[3] == 0x75) && \
          (buf[4] == 0x8e) && (buf[5] == 0x66) && \
          (buf[6] == 0xcf) && (buf[7] == 0x11))

//AVI header format
#define IS_AVI_HDR(buf)                         \
         ((buf[0] == 'R') && (buf[1] == 'I') && \
          (buf[2] == 'F') && (buf[3] == 'F') && \
          (buf[8] == 'A') && (buf[9] == 'V') && \
          (buf[10] == 'I'))

//MIDI header format
#define IS_MIDI_HDR(buf)                         \
         ((buf[0] == 'M') && (buf[1] == 'T') && \
          (buf[2] == 'h') && (buf[3] == 'd') && \
          (buf[4] == 0)  && (buf[5] == 0) && \
          (buf[6] == 0)  && (buf[7] == 6))

int ONScripterLabel::playSound(const char *filename, int format, bool loop_flag, int channel)
{
    if ( !audio_open_flag ) return SOUND_NONE;

    long length = script_h.cBR->getFileLength( filename );
    if (length == 0) return SOUND_NONE;

    //Mion: account for mode_wave_demo setting
    //(i.e. if not set, then don't play non-bgm wave/ogg during skip mode)
    if (!mode_wave_demo_flag &&
        ( (skip_mode & SKIP_NORMAL) || ctrl_pressed_status )) {
        if ((format & (SOUND_OGG | SOUND_WAVE)) &&
            ((channel < ONS_MIX_CHANNELS) || (channel == MIX_WAVE_CHANNEL) ||
             (channel == MIX_CLICKVOICE_CHANNEL)))
            return SOUND_NONE;
    }

    unsigned char *buffer;

    if ((format & (SOUND_MP3 | SOUND_OGG_STREAMING)) && 
        (length == music_buffer_length) &&
        music_buffer ){
        buffer = music_buffer;
    }
    else{
        buffer = new(std::nothrow) unsigned char[length];
        if (buffer == NULL) {
            snprintf(script_h.errbuf, MAX_ERRBUF_LEN,
                     "failed to load sound file [%s] (%lu bytes)",
                     filename, length);
            errorAndCont( script_h.errbuf, "unable to allocate buffer", "Memory Issue" );
            return SOUND_NONE;
        }
        script_h.cBR->getFile( filename, buffer );
    }

    if (format & (SOUND_OGG | SOUND_OGG_STREAMING)){
        Mix_Chunk* chunk = Mix_LoadWAV_RW(SDL_RWFromMem(buffer, length), 1);
        if (chunk == NULL)
        {
            fprintf(stderr, "Issue loading ogg file: %s\n", SDL_GetError());
            return SOUND_OTHER;
        }

        return playWave(chunk, format, loop_flag, channel) == 0 ? SOUND_OGG : SOUND_OTHER;
    }

    /* check for WMA (i.e. ASF header format) */
    if ( IS_ASF_HDR(buffer) ){
        snprintf(script_h.errbuf, MAX_ERRBUF_LEN,
        "sound file '%s' is in WMA format, skipping", filename);
        errorAndCont(script_h.errbuf);
        delete[] buffer;
        return SOUND_OTHER;
    }

    /* check for AVI header format */
    if ( IS_AVI_HDR(buffer) ){
        snprintf(script_h.errbuf, MAX_ERRBUF_LEN,
        "sound file '%s' is in AVI format, skipping", filename);
        errorAndCont(script_h.errbuf);
        delete[] buffer;
        return SOUND_OTHER;
    }

    if (format & SOUND_WAVE){
        if (strncmp((char*) buffer, "RIFF", 4) != 0) {
            // bad (encrypted?) header; need to recreate
            // assumes the first 128 bytes are bad (encrypted)
            // _and_ that the file contains uncompressed PCM data
            char *fmtname = new char[strlen(filename) + strlen(".fmt") + 1];
            sprintf(fmtname, "%s.fmt", filename);
            unsigned int fmtlen = script_h.cBR->getFileLength( fmtname );
            if ( fmtlen >= 8) {
                // a file called filename + ".fmt" exists, of appropriate size;
                // read fmt info
                unsigned char *buffer2 = new unsigned char[fmtlen];
                script_h.cBR->getFile( fmtname, buffer2 );

                int channels, bits;
                unsigned long rate=0, data_length=0;

                channels = buffer2[0];
                for (int i=5; i>1; i--) {
                    rate = (rate << 8) + buffer2[i];
                }
                bits = buffer2[6];
                if (fmtlen >= 12) {
                    // read the data_length
                    for (int i=11; i>7; i--) {
                        data_length = (data_length << 8) + buffer2[i];
                    }
                } else {
                    // no data_length provided, fake it from the buffer length
                    data_length = length - sizeof(WAVE_HEADER) - sizeof(WAVE_DATA_HEADER);
                }
                unsigned char fill = 0;
                if (bits == 8) fill = 128;
                for (int i=0; (i<128 && i<length); i++) {
                    //clear the first 128 bytes (encryption noise)
                    buffer[i] = fill;
                }
                if (fmtlen > 12) {
                    setupWaveHeader(buffer, channels, bits, rate, data_length,
                                    fmtlen - 12, buffer2 + 12);
                } else {
                    setupWaveHeader(buffer, channels, bits, rate, data_length);
                }
                if ((bits == 8) && (fmtlen < 12)) {
                    //hack: clear likely "pad bytes" at the end of the buffer
                    //      (only on 8-bit samples when the fmt file doesn't
                    //      include the data length)
                    int i = 1;
                    while (i<5 && buffer[length-i] == 0) {
                        buffer[length - i] = fill;
                        i++;
                    }
                }
                delete[] buffer2;
            }
            delete[] fmtname;
        }
        Mix_Chunk *chunk = Mix_LoadWAV_RW(SDL_RWFromMem(buffer, length), 1);
        if (playWave(chunk, format, loop_flag, channel) == 0){
            delete[] buffer;
            return SOUND_WAVE;
        }
    }

    if ((format & SOUND_MP3) &&
        !(IS_MIDI_HDR(buffer) && (format & SOUND_SEQMUSIC))){ //bypass MIDIs
        Mix_Chunk* chunk = Mix_LoadWAV_RW(SDL_RWFromMem(buffer, length), 1);
        if (chunk == NULL)
        {
            fprintf(stderr, "Issue loading mp3 file: %s\n", SDL_GetError());
            return SOUND_OTHER;
        }

        return playWave(chunk, format, loop_flag, channel) == 0 ? SOUND_SEQMUSIC : SOUND_OTHER;
    }

    if (format & SOUND_SEQMUSIC){
        Mix_Chunk* chunk = Mix_LoadWAV_RW(SDL_RWFromMem(buffer, length), 1);
        if (chunk == NULL)
        {
            fprintf(stderr, "Issue loading Midi file: %s\n", SDL_GetError());
            return SOUND_OTHER;
        }

        return playWave(chunk, format, loop_flag, channel) == 0 ? SOUND_SEQMUSIC : SOUND_OTHER;
    }

    delete[] buffer;

    return SOUND_OTHER;
}

void ONScripterLabel::playCDAudio()
{
    if (!audio_open_flag) return;

    if ( cdaudio_flag ){
        if ( cdrom_info ){
            int length = cdrom_info->track[current_cd_track - 1].length / 75;
            SDL_CDPlayTracks( cdrom_info, current_cd_track - 1, 0, 1, 0 );
            timer_cdaudio_id = SDL_AddTimer( length * 1000, cdaudioCallback, NULL );
        }
    }
    else{
        //if CD audio is not available, search the "cd" subfolder
        //for a file named "track01.mp3" or similar, depending on the
        //track number; check for mp3, ogg and wav files
        char filename[256];
        sprintf( filename, "cd\\track%2.2d.mp3", current_cd_track );
        int ret = playSound( filename, SOUND_MP3, cd_play_loop_flag, MIX_BGM_CHANNEL);
        if (ret == SOUND_MP3) return;

        sprintf( filename, "cd\\track%2.2d.ogg", current_cd_track );
        ret = playSound( filename, SOUND_OGG, cd_play_loop_flag, MIX_BGM_CHANNEL);
        if (ret == SOUND_OGG) return;

        sprintf( filename, "cd\\track%2.2d.wav", current_cd_track );
        ret = playSound( filename, SOUND_WAVE, cd_play_loop_flag, MIX_BGM_CHANNEL );
    }
}

int ONScripterLabel::playWave(Mix_Chunk *chunk, int format, bool loop_flag, int channel)
{
    Mix_Pause( channel );
    if ( wave_sample[channel] ) {
      Mix_ChannelFinished(NULL);
      Mix_FreeChunk( wave_sample[channel] );
    }
    wave_sample[channel] = chunk;

    if (!chunk) return -1;

    if      (channel < ONS_MIX_CHANNELS)
        Mix_Volume( channel, !volume_on_flag? 0 : channelvolumes[channel] * 128 / 100 );
    else if (channel == MIX_CLICKVOICE_CHANNEL)
        Mix_Volume( channel, !volume_on_flag? 0 : se_volume * 128 / 100 );
    else if (channel == MIX_BGM_CHANNEL)
        Mix_Volume( channel, !volume_on_flag? 0 : music_volume * 128 / 100 );
    else
        Mix_Volume( channel, !volume_on_flag? 0 : se_volume * 128 / 100 );

    if (!(format & SOUND_PRELOAD)) {
        Mix_ChannelFinished(waveCallback);
        Mix_PlayChannel(channel, wave_sample[channel], loop_flag ? -1 : 0);
    }

    return 0;
}

int ONScripterLabel::playExternalMusic(bool loop_flag)
{
    int music_looping = loop_flag ? -1 : 0;
#ifdef LINUX
    signal(SIGCHLD, musicCallback);
    if (music_cmd) music_looping = 0;
#endif

    Mix_SetMusicCMD(music_cmd);

    char music_filename[256];
    sprintf(music_filename, "%s%s", script_h.save_path, TMP_MUSIC_FILE);
    if ((music_info = Mix_LoadMUS(music_filename)) == NULL){
        snprintf(script_h.errbuf, MAX_ERRBUF_LEN,
                 "can't load music file %s", music_filename );
        errorAndCont(script_h.errbuf);
        return -1;
    }

    // Mix_VolumeMusic( music_volume );
    Mix_PlayMusic(music_info, music_looping);

    return 0;
}

int ONScripterLabel::playSequencedMusic(bool loop_flag)
{
    Mix_SetMusicCMD(seqmusic_cmd);

    char seqmusic_filename[256];
    sprintf(seqmusic_filename, "%s%s", script_h.save_path, TMP_SEQMUSIC_FILE);
    seqmusic_info = Mix_LoadMUS(seqmusic_filename);
    if (seqmusic_info == NULL) {
        snprintf(script_h.errbuf, MAX_ERRBUF_LEN,
                 "error in sequenced music file %s", seqmusic_filename );
        errorAndCont(script_h.errbuf, Mix_GetError());
        return -1;
    }

    int seqmusic_looping = loop_flag ? -1 : 0;

#ifdef LINUX
    signal(SIGCHLD, seqmusicCallback);
    if (seqmusic_cmd) seqmusic_looping = 0;
#endif

    Mix_VolumeMusic(!volume_on_flag? 0 : music_volume);
#if defined(MACOSX) //insani
    // Emulate looping on MacOS ourselves to work around bug in SDL_Mixer
    seqmusic_looping = 0;
    Mix_PlayMusic(seqmusic_info, seqmusic_looping);
    timer_seqmusic_id = SDL_AddTimer(1000, seqmusicSDLCallback, NULL);
#else
    Mix_PlayMusic(seqmusic_info, seqmusic_looping);
#endif
    current_cd_track = -2;

    return 0;
}

int ONScripterLabel::playingMusic()
{
    if (audio_open_flag && 
        ( (Mix_GetMusicHookData() != NULL) ||
          (Mix_Playing(MIX_BGM_CHANNEL) == 1) ||
          (Mix_PlayingMusic() == 1) ))
        return 1;
    else
        return 0;
}

int ONScripterLabel::setCurMusicVolume( int volume )
{
    if (!audio_open_flag) return 0;

    if (music_struct.voice_sample && *(music_struct.voice_sample))
        volume /= 2;
    if (Mix_GetMusicHookData() != NULL) { // for streamed MP3 & OGG
        if ( mp3_sample ) Mix_Volume(mp3_channel, !volume_on_flag ? 0 : volume);
        else music_struct.volume = volume; // ogg
    } else if (Mix_Playing(MIX_BGM_CHANNEL) == 1) { // wave
        Mix_Volume( MIX_BGM_CHANNEL, !volume_on_flag? 0 : volume * 128 / 100 );
    } else if (Mix_PlayingMusic() == 1) { // midi
        Mix_VolumeMusic( !volume_on_flag? 0 : volume * 128 / 100 );
    }

    return 0;
}

int ONScripterLabel::setVolumeMute( bool do_mute )
{
    if (!audio_open_flag) return 0;

    int music_vol = music_volume;
    if (music_struct.voice_sample && *(music_struct.voice_sample)) //bgmdown
        music_vol /= 2;
    if (Mix_GetMusicHookData() != NULL) { // for streamed MP3 & OGG
        if ( mp3_sample ) Mix_Volume(mp3_channel, do_mute ? 0 : music_vol); // mp3
        if ( async_movie ) SMPEG_setvolume( async_movie, do_mute? 0 : music_vol ); // async mpeg
        else music_struct.is_mute = do_mute; // ogg
    } else if (Mix_Playing(MIX_BGM_CHANNEL) == 1) { // wave
        Mix_Volume( MIX_BGM_CHANNEL, do_mute? 0 : music_vol * 128 / 100 );
    } else if (Mix_PlayingMusic() == 1) { // midi
        Mix_VolumeMusic( do_mute? 0 : music_vol * 128 / 100 );
    }
    for ( int i=1 ; i<ONS_MIX_CHANNELS ; i++ ) {
        if ( wave_sample[i] )
            Mix_Volume( i, do_mute? 0 : channelvolumes[i] * 128 / 100 );
     }
    if ( wave_sample[MIX_LOOPBGM_CHANNEL0] )
        Mix_Volume( MIX_LOOPBGM_CHANNEL0, do_mute? 0 : se_volume * 128 / 100 );
    if ( wave_sample[MIX_LOOPBGM_CHANNEL1] )
        Mix_Volume( MIX_LOOPBGM_CHANNEL1, do_mute? 0 : se_volume * 128 / 100 );

    return 0;
}

int ONScripterLabel::playMPEG( const char *filename, bool async_flag, bool use_pos, int xpos, int ypos, int width, int height )
{
    int ret = 0;

    if (audio_open_flag) Mix_CloseAudio();

    FFMpegWrapper avi;
    if (avi.initialize(this, m_window, filename, audio_open_flag, false) == 0) {
        if (avi.play(true)) return 1;
    }
    //delete[] absolute_filename;

    if (audio_open_flag) {
        Mix_CloseAudio();
        openAudio();
    }
//#ifndef MP3_MAD
//    bool different_spec = false;
//    if (async_movie) stopMovie(async_movie);
//    async_movie = NULL;
//    if (movie_buffer) delete[] movie_buffer;
//    movie_buffer = NULL;
//    if (surround_rects) delete[] surround_rects;
//    surround_rects = NULL;
//
//    unsigned long length = script_h.cBR->getFileLength( filename );
//
//    if (length == 0) {
//        snprintf(script_h.errbuf, MAX_ERRBUF_LEN,
//                 "couldn't load movie '%s'", filename);
//        errorAndCont(script_h.errbuf);
//        return 0;
//    }
//
//    movie_buffer = new unsigned char[length];
//    script_h.cBR->getFile( filename, movie_buffer );
//
//    /* check for AVI header format */
//    if ( IS_AVI_HDR(movie_buffer) ){
//        snprintf(script_h.errbuf, MAX_ERRBUF_LEN,
//                 "movie file '%s' is in AVI format", filename);
//        errorAndCont(script_h.errbuf);
//        if (movie_buffer) delete[] movie_buffer;
//        movie_buffer = NULL;
//        return 0;
//    }
//
//    SMPEG_Info mpeg_info;
//    SMPEG *mpeg_sample = SMPEG_new_rwops( SDL_RWFromMem( movie_buffer, length ), NULL, 1, 0 );
//    char *errstr = SMPEG_error( mpeg_sample );
//    if (errstr){
//        
//        snprintf(script_h.errbuf, MAX_ERRBUF_LEN,
//                 "SMPEG error on '%s'", filename);
//        errorAndCont(script_h.errbuf, errstr);
//        if (movie_buffer) delete[] movie_buffer;
//        movie_buffer = NULL;
//        return 0;
//    }
//    else {
//        SMPEG_Info info;
//        SMPEG_getinfo(mpeg_sample, &info);
//        if (info.has_audio){
//            stopBGM( false );
//            SMPEG_enableaudio( mpeg_sample, 0 );
//
//            if ( audio_open_flag ){
//                //Mion - SMPEG doesn't handle different audio spec well, so
//                // we might reset the SDL mixer for this video playback
//                SDL_AudioSpec wanted;
//                SMPEG_wantedSpec( mpeg_sample, &wanted );
//                //printf("SMPEG wants audio: %d Hz %d bit %s\n", wanted.freq,
//                //       (wanted.format&0xFF),
//                //       (wanted.channels > 1) ? "stereo" : "mono");
//                if ((wanted.format != audio_format.format) ||
//                    (wanted.freq != audio_format.freq)) {
//                    different_spec = true;
//                    Mix_CloseAudio();
//                    openAudio(wanted.freq, wanted.format, wanted.channels);
//                    if (!audio_open_flag) {
//                        // didn't work, use the old settings
//                        openAudio();
//                        different_spec = false;
//                    }
//                }
//                SMPEG_actualSpec( mpeg_sample, &audio_format );
//                SMPEG_enableaudio( mpeg_sample, 1 );
//            }
//        } else {
//            different_spec = false;
//        }
//
//        int texture_width = (info.width + 15) & ~15;
//        int texture_height = (info.height + 15) & ~15;
//        frame_texture = SDL_CreateTexture(m_window->GetRenderer(), SDL_PIXELFORMAT_YV12, SDL_TEXTUREACCESS_STREAMING, texture_width, texture_height);
//        int frame_number_local = frame_number = 0;
//
//        SMPEG_enablevideo( mpeg_sample, 1 );
//        SMPEG_setdisplay( mpeg_sample, SmpegDisplayCallback, this, frame_mutex);
//
//        if (use_pos) {
//            smpeg_scale_x = width;
//            smpeg_scale_y = height;
//            smpeg_move_x = xpos;
//            smpeg_move_y = ypos;
//        }
//        else if (nomovieupscale_flag && (info.width < screen_width) &&
//                 (info.height < screen_height)) {
//            //"no-movie-upscale" set, so use its native
//            //width/height & center within the screen
//            smpeg_scale_x = info.width;
//            smpeg_scale_y = info.height;
//            smpeg_move_x = (screen_width - info.width) / 2;
//            smpeg_move_y = (screen_height - info.height) / 2;
//        }
//#ifdef RCA_SCALE
//        //center the movie on the screen, using standard aspect ratio
//        else if ( (scr_stretch_x > 1.0) || (scr_stretch_y > 1.0) ) {
//            width = ExpandPos(script_width);
//            height = ExpandPos(script_height);
//            SMPEG_scaleXY( mpeg_sample, width, height );
//            SMPEG_move( mpeg_sample, (screen_width - width) / 2,
//                       (screen_height - height) / 2 );
//        }
//#endif
//
//        if (info.has_audio){
//            int volume = !volume_on_flag ? 0 : music_volume;
//            SMPEG_setvolume( mpeg_sample, volume);
//            Mix_HookMusic( mp3callback, mpeg_sample );
//        }
//
//        surround_rects = new SDL_Rect[4];
//        for (int i=0; i<4; ++i) {
//            surround_rects[i].x = surround_rects[i].y = 0;
//            surround_rects[i].w = surround_rects[i].h = 0;
//        }
//
//        if (use_pos) {
//            async_movie_rect.x = xpos;
//            async_movie_rect.y = ypos;
//            async_movie_rect.w = width;
//            async_movie_rect.h = height;
//
//            //sur_rect[0] = { 0, 0, screen_width, ypos };
//            //sur_rect[1] = { 0, ypos, xpos, height };
//            //sur_rect[2] = { xpos + width, ypos, screen_width - (xpos + width), height };
//            //sur_rect[3] = { 0, ypos + height, screen_width, screen_height - (ypos + height) };
//            surround_rects[0].w = surround_rects[3].w = screen_width;
//            surround_rects[0].h = surround_rects[1].y = surround_rects[2].y = ypos;
//            surround_rects[1].w = xpos;
//            surround_rects[1].h = surround_rects[2].h = height;
//            surround_rects[2].x = xpos + width;
//            surround_rects[2].w = screen_width - (xpos + width);
//            surround_rects[3].y = ypos + height;
//            surround_rects[3].h = screen_height - (ypos + height);
//        } else {
//            async_movie_rect.x = 0;
//            async_movie_rect.y = 0;
//            async_movie_rect.w = screen_width;
//            async_movie_rect.h = screen_height;
//        }
//
//        if (movie_loop_flag)
//            SMPEG_loop( mpeg_sample, -1 );
//        SMPEG_play( mpeg_sample );
//
//        if (async_flag){
//            async_movie = mpeg_sample;
//            if (!info.has_audio && movie_loop_flag){
//                timer_silentmovie_id = SDL_AddTimer(100, silentmovieCallback,
//                                                    (void*)&async_movie);
//            }
//            return 0;
//        }
//
//        bool done_flag = false;
//        while( !done_flag ){
//            if (SMPEG_status(mpeg_sample) != SMPEG_PLAYING){
//                if (movie_loop_flag)
//                    SMPEG_play( mpeg_sample );
//                else
//                    break;
//            }
//
//            SDL_Event event;
//
//            while (m_window->PollEvents(event)) {
//                switch (event.type){
//                  case SDL_KEYUP:
//                    if ( ((SDL_KeyboardEvent *)&event)->keysym.sym == SDLK_RETURN ||
//                         ((SDL_KeyboardEvent *)&event)->keysym.sym == SDLK_SPACE ||
//                         ((SDL_KeyboardEvent *)&event)->keysym.sym == SDLK_ESCAPE )
//                        done_flag = movie_click_flag;
//                    else if ( ((SDL_KeyboardEvent *)&event)->keysym.sym == SDLK_f ){
//#ifndef PSP
//                        if ( SDL_SetWindowFullscreen(m_window->GetWindow(), SDL_WINDOW_FULLSCREEN_DESKTOP) < 0) {
//                            SMPEG_pause( mpeg_sample );
//                            SDL_FreeSurface(screen_surface);
//                            if ( fullscreen_mode )
//                                screen_surface = m_window->SetVideoMode( screen_width, screen_height, screen_bpp, false );
//                            else
//                                screen_surface = m_window->SetVideoMode( screen_width, screen_height, screen_bpp, true );
//                            SMPEG_setdisplay( mpeg_sample, SmpegDisplayCallback, this, NULL );
//                            SMPEG_play( mpeg_sample );
//                        }
//#endif
//                        fullscreen_mode = !fullscreen_mode;
//                    }
//                    else if ( ((SDL_KeyboardEvent *)&event)->keysym.sym == SDLK_m ){
//                        volume_on_flag = !volume_on_flag;
//                        SMPEG_setvolume( mpeg_sample, !volume_on_flag? 0 : music_volume );
//                        printf("turned %s volume mute\n", !volume_on_flag?"on":"off");
//                    }
//                    break;
//                  case SDL_QUIT:
//                    ret = 1;
//                    done_flag = true;
//                    break;
//                  case SDL_MOUSEBUTTONUP:
//                    done_flag = movie_click_flag;
//                    break;
//                  default:
//                    break;
//                }
//                //SDL_Delay( 5 );
//            }
//
//
//            if (frame_number_local < frame_number) {
//                SDL_mutexP(frame_mutex);
//                SDL_assert(frame->image_width == texture_width);
//                SDL_assert(frame->image_height == texture_height);
//                SDL_UpdateTexture(frame_texture, NULL, frame->image, frame->image_width);
//                frame_number = frame_number;
//                SDL_mutexV(frame_mutex);
//
//                DisplayTexture(frame_texture);
//            }
//        }
//        ctrl_pressed_status = 0;
//
//        stopMovie(mpeg_sample);
//
//        if (different_spec) {
//            //restart mixer with the old audio spec
//            Mix_CloseAudio();
//            openAudio();
//        }
//    }
//#else
//    errorAndCont( "mpeg video playback is disabled." );
//#endif
//
    return ret;
}

int ONScripterLabel::playAVI( const char *filename, bool click_flag )
{
#ifdef USE_AVIFILE
    if ( audio_open_flag ) Mix_CloseAudio();

    FFMpegWrapper avi;
    if ( avi.initialize(this, m_window, filename, audio_open_flag, false) == 0) {
        if (avi.play( click_flag )) return 1;
    }
    //delete[] absolute_filename;

    if ( audio_open_flag ){
        Mix_CloseAudio();
        openAudio();
    }

#if 0
    char *absolute_filename = new char[ strlen(archive_path) + strlen(filename) + 1 ];
    sprintf( absolute_filename, "%s%s", archive_path, filename );
    for ( unsigned int i=0 ; i<strlen( absolute_filename ) ; i++ )
        if ( absolute_filename[i] == '/' ||
             absolute_filename[i] == '\\' )
            absolute_filename[i] = DELIMITER;

    if ( audio_open_flag ) Mix_CloseAudio();

    AVIWrapper *avi = new AVIWrapper();
    if ( avi->init( absolute_filename, false ) == 0 &&
         avi->initAV( screen_surface, audio_open_flag ) == 0 ){
        if (avi->play( click_flag )) return 1;
    }
    delete avi;
    delete[] absolute_filename;

    if ( audio_open_flag ){
        Mix_CloseAudio();
        openAudio();
    }
#endif
#else
    errorAndCont( "avi: avi video playback is disabled." );
#endif

    return 0;
}

void ONScripterLabel::stopMovie(SMPEG *mpeg)
{
#ifndef MP3_MAD
    if (mpeg) {
        SMPEG_Info info;
        SMPEG_getinfo(mpeg, &info);
        SMPEG_stop( mpeg );
        if (info.has_audio){
            Mix_HookMusic( NULL, NULL );
        }
        SMPEG_delete( mpeg );

        dirty_rect.add( async_movie_rect );
    }

    if (movie_buffer) delete[] movie_buffer;
    movie_buffer = NULL;
    if (surround_rects) delete[] surround_rects;
    surround_rects = NULL;
#endif
}

void ONScripterLabel::stopBGM( bool continue_flag )
{
    if ( cdaudio_flag && cdrom_info ){
        extern SDL_TimerID timer_cdaudio_id;

        clearTimer( timer_cdaudio_id );
        if (SDL_CDStatus( cdrom_info ) >= CD_PLAYING )
            SDL_CDStop( cdrom_info );
    }

    if ( mp3_sample ){
        Mix_Pause(MIX_BGM_CHANNEL);
        Mix_ChannelFinished(NULL);
        Mix_FreeChunk(mp3_sample);
        mp3_sample = NULL;
    }

    if (music_struct.ovi){
        Mix_HaltMusic();
        Mix_HookMusic( NULL, NULL );
        closeOggVorbis(music_struct.ovi);
        music_struct.ovi = NULL;
    }

    if ( wave_sample[MIX_BGM_CHANNEL] ){
        Mix_Pause( MIX_BGM_CHANNEL );
        Mix_ChannelFinished(NULL);
        Mix_FreeChunk( wave_sample[MIX_BGM_CHANNEL] );
        wave_sample[MIX_BGM_CHANNEL] = NULL;
    }

    if ( !continue_flag ){
        setStr( &music_file_name, NULL );
        music_play_loop_flag = false;
        if ( music_buffer ){
            delete[] music_buffer;
            music_buffer = NULL;
        }
    }

    if ( seqmusic_info ){

#if defined(MACOSX) //insani
        clearTimer( timer_seqmusic_id );
#endif

        ext_music_play_once_flag = true;
        Mix_HaltMusic();
        Mix_FreeMusic( seqmusic_info );
        seqmusic_info = NULL;
    }
    if ( !continue_flag ){
        setStr( &seqmusic_file_name, NULL );
        seqmusic_play_loop_flag = false;
    }

    if ( music_info ){
        ext_music_play_once_flag = true;
        Mix_HaltMusic();
        Mix_FreeMusic( music_info );
        music_info = NULL;
    }

    if ( !continue_flag ) current_cd_track = -1;
}

void ONScripterLabel::stopDWAVE( int channel )
{
    if (!audio_open_flag) return;

    //avoid stopping dwave outside array
    if (channel < 0) channel = 0;
    else if (channel >= ONS_MIX_CHANNELS) channel = ONS_MIX_CHANNELS-1;

    if ( wave_sample[channel] ){
        Mix_Pause( channel );
        if ( !channel_preloaded[channel] || channel == 0 ){
            //don't free preloaded channels, _except_:
            //always free voice channel, for now - could be
            //messy for bgmdownmode and/or voice-waiting FIXME
            Mix_ChannelFinished(NULL);
            Mix_FreeChunk( wave_sample[channel] );
            wave_sample[channel] = NULL;
            channel_preloaded[channel] = false;
        }
    }
    if ((channel == 0) && bgmdownmode_flag)
        setCurMusicVolume( music_volume );
}

void ONScripterLabel::stopAllDWAVE()
{
    if (!audio_open_flag) return;

    for (int ch=0; ch<ONS_MIX_CHANNELS ; ch++) {
        if ( wave_sample[ch] ){
            Mix_Pause( ch );
            if ( !channel_preloaded[ch] || ch == 0 ){
                //always free voice channel sample, for now - could be
                //messy for bgmdownmode and/or voice-waiting FIXME
                Mix_ChannelFinished(NULL);
                Mix_FreeChunk( wave_sample[ch] );
                wave_sample[ch] = NULL;
            }
        }
    }
    // just in case the bgm was turned down for the voice channel,
    // set the bgm volume back to normal
    if (bgmdownmode_flag)
        setCurMusicVolume( music_volume );
}

void ONScripterLabel::playClickVoice()
{
    if      ( clickstr_state == CLICK_NEWPAGE ){
        if ( clickvoice_file_name[CLICKVOICE_NEWPAGE] )
            playSound(clickvoice_file_name[CLICKVOICE_NEWPAGE],
                      SOUND_WAVE|SOUND_OGG, false, MIX_CLICKVOICE_CHANNEL);
    }
    else if ( clickstr_state == CLICK_WAIT ){
        if ( clickvoice_file_name[CLICKVOICE_NORMAL] )
            playSound(clickvoice_file_name[CLICKVOICE_NORMAL],
                      SOUND_WAVE|SOUND_OGG, false, MIX_CLICKVOICE_CHANNEL);
    }
}

void setupWaveHeader( unsigned char *buffer, int channels, int bits,
                      unsigned long rate, unsigned long data_length,
                      unsigned int extra_bytes, unsigned char *extra_ptr )
{
    memcpy( header.chunk_riff, "RIFF", 4 );
    unsigned long riff_length = sizeof(WAVE_HEADER) + sizeof(WAVE_DATA_HEADER) +
                                data_length + extra_bytes - 8;
    header.riff_length[0] = riff_length & 0xff;
    header.riff_length[1] = (riff_length >> 8) & 0xff;
    header.riff_length[2] = (riff_length >> 16) & 0xff;
    header.riff_length[3] = (riff_length >> 24) & 0xff;
    memcpy( header.fmt_id, "WAVEfmt ", 8 );
    header.fmt_size[0] = 0x10 + extra_bytes;
    header.fmt_size[1] = header.fmt_size[2] = header.fmt_size[3] = 0;
    header.data_fmt[0] = 1; header.data_fmt[1] = 0; // PCM format
    header.channels[0] = channels; header.channels[1] = 0;
    header.frequency[0] = rate & 0xff;
    header.frequency[1] = (rate >> 8) & 0xff;
    header.frequency[2] = (rate >> 16) & 0xff;
    header.frequency[3] = (rate >> 24) & 0xff;

    int sample_byte_size = channels * bits / 8;
    unsigned long byte_size = sample_byte_size * rate;
    header.byte_size[0] = byte_size & 0xff;
    header.byte_size[1] = (byte_size >> 8) & 0xff;
    header.byte_size[2] = (byte_size >> 16) & 0xff;
    header.byte_size[3] = (byte_size >> 24) & 0xff;
    header.sample_byte_size[0] = sample_byte_size;
    header.sample_byte_size[1] = 0;
    header.sample_bit_size[0] = bits;
    header.sample_bit_size[1] = 0;

    memcpy( data_header.chunk_id, "data", 4 );
    data_header.data_length[0] = (char)(data_length & 0xff);
    data_header.data_length[1] = (char)((data_length >> 8) & 0xff);
    data_header.data_length[2] = (char)((data_length >> 16) & 0xff);
    data_header.data_length[3] = (char)((data_length >> 24) & 0xff);

    memcpy( buffer, &header, sizeof(header) );
    if (extra_bytes > 0) {
        if (extra_ptr != NULL)
            memcpy( buffer+sizeof(header), extra_ptr, extra_bytes );
        else
            memset( buffer+sizeof(header), 0, extra_bytes );
    }
    memcpy( buffer+sizeof(header)+extra_bytes, &data_header, sizeof(data_header) );
}
#ifdef USE_OGG_VORBIS
static size_t oc_read_func(void *ptr, size_t size, size_t nmemb, void *datasource)
{
    OVInfo *ogg_vorbis_info = (OVInfo*)datasource;

    size_t len = size*nmemb;
    if ((size_t)ogg_vorbis_info->pos+len > (size_t)ogg_vorbis_info->length) 
        len = (size_t)(ogg_vorbis_info->length - ogg_vorbis_info->pos);
    memcpy(ptr, ogg_vorbis_info->buf+ogg_vorbis_info->pos, len);
    ogg_vorbis_info->pos += len;

    return len;
}

static int oc_seek_func(void *datasource, ogg_int64_t offset, int whence)
{
    OVInfo *ogg_vorbis_info = (OVInfo*)datasource;

    ogg_int64_t pos = 0;
    if (whence == 0)
        pos = offset;
    else if (whence == 1)
        pos = ogg_vorbis_info->pos + offset;
    else if (whence == 2)
        pos = ogg_vorbis_info->length + offset;

    if (pos < 0 || pos > ogg_vorbis_info->length) return -1;

    ogg_vorbis_info->pos = pos;

    return 0;
}

static int oc_close_func(void *datasource)
{
    return 0;
}

static long oc_tell_func(void *datasource)
{
    OVInfo *ogg_vorbis_info = (OVInfo*)datasource;

    return (long)ogg_vorbis_info->pos;
}
#endif
OVInfo *ONScripterLabel::openOggVorbis( unsigned char *buf, long len, int &channels, int &rate )
{
    OVInfo *ovi = NULL;

#ifdef USE_OGG_VORBIS
    ovi = new OVInfo();

    ovi->buf = buf;
    ovi->decoded_length = 0;
    ovi->length = len;
    ovi->pos = 0;

    ov_callbacks oc;
    oc.read_func  = oc_read_func;
    oc.seek_func  = oc_seek_func;
    oc.close_func = oc_close_func;
    oc.tell_func  = oc_tell_func;
    if (ov_open_callbacks(ovi, &ovi->ovf, NULL, 0, oc) < 0){
        delete ovi;
        return NULL;
    }

    vorbis_info *vi = ov_info( &ovi->ovf, -1 );
    if (vi == NULL){
        ov_clear(&ovi->ovf);
        delete ovi;
        return NULL;
    }

    channels = vi->channels;
    rate = vi->rate;

    ovi->cvt.buf = NULL;
    ovi->cvt_len = 0;
    SDL_BuildAudioCVT(&ovi->cvt,
                      AUDIO_S16, channels, rate,
                      audio_format.format, audio_format.channels, audio_format.freq);
    ovi->mult1 = 10;
    ovi->mult2 = (int)(ovi->cvt.len_ratio*10.0);

    ovi->decoded_length = (long)(ov_pcm_total(&ovi->ovf, -1) * channels * 2);
#endif

    return ovi;
}

int ONScripterLabel::closeOggVorbis(OVInfo *ovi)
{
    if (ovi->buf){
        ovi->buf = NULL;
#ifdef USE_OGG_VORBIS
        ovi->length = 0;
        ovi->pos = 0;
        ov_clear(&ovi->ovf);
#endif
    }
    if (ovi->cvt.buf){
        delete[] ovi->cvt.buf;
        ovi->cvt.buf = NULL;
        ovi->cvt_len = 0;
    }
    delete ovi;

    return 0;
}
