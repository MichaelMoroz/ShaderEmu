/*
snd_media.c - MP3 and Ogg Vorbis sounds and streams through libmedia (docs/system/media.md), on the
worker cores when a whole sound is loaded. Opus has no decoder there.
*/
#include <stdlib.h>
#include "common.h"
#include "soundlib.h"
#include "media.h"
#include "se_server.h"

#define LOAD_WORKERS 64
int hlglServerWorker( void );
void hlglJobsWait( void );
int hlglRingsOn( void ), SE_ServerServing( void );

// The workers a decode may have: the studio models' wait first, and the server's is not one (sv_shaderemu.c).
int SE_MediaWorkers( void )
{
	int server = hlglServerWorker();

	// (none on the server's or the drawing's worker, while the rings hold the workers, or while this
	// core makes the server's ask: its jobs may be out)
	if( SE_OffCore0( ) || hlglRingsOn( ) || SE_ServerServing( ))
		return 0;
	hlglJobsWait();
	return server > 1 ? server - 1 : server ? 0 : LOAD_WORKERS;
}

static qboolean load( const char *name, const byte *buffer, fs_offset_t filesize )
{
	struct media_audio_info info;
	struct media_audio *audio;
	const char *why = NULL;
	int16_t *to, *grown;
	uint32_t frames, most;
	int n;

	if( !( audio = media_audio_open( buffer, (uint32_t)filesize, &why )))
	{
		Con_DPrintf( S_ERROR "%s: %s\n", name, why ? why : "not a sound libmedia reads" );
		return false;
	}
	media_audio_get_info( audio, &info );
	if( info.channels < 1 || info.channels > 2 )
	{
		media_audio_close( audio );
		return false;
	}
	if( info.frames )
	{
		// a 16-byte aligned destination lets every worker decode its pieces in place
		if( posix_memalign( (void **)&to, 16, (size_t)info.frames * info.channels * 2 ))
			to = NULL;
		frames = to ? media_audio_decode_all( audio, to, info.frames, SE_MediaWorkers( )) : 0;
	}
	else
	{
		for( to = NULL, frames = most = 0;; frames += n )
		{
			if( frames + 4096 > most )
			{
				most = most ? most * 2 : 65536;
				if( !( grown = realloc( to, (size_t)most * info.channels * 2 )))
					break;
				to = grown;
			}
			if(( n = media_audio_read( audio, to + frames * info.channels, 4096 )) <= 0 )
				break;
		}
	}
	media_audio_close( audio );
	if( !frames )
	{
		free( to );
		return false;
	}
	sound.channels = info.channels;
	sound.rate = info.rate;
	sound.width = 2;
	sound.type = WF_PCMDATA;
	sound.samples = frames;
	sound.size = (size_t)frames * info.channels * 2;
	sound.wav = (byte *)Mem_Malloc( host.soundpool, sound.size );
	memcpy( sound.wav, to, sound.size );
	free( to );
	SetBits( sound.flags, SOUND_RESAMPLE );
	return true;
}

qboolean Sound_LoadMPG( const char *name, const byte *buffer, fs_offset_t filesize ) { return load( name, buffer, filesize ); }
qboolean Sound_LoadOggVorbis( const char *name, const byte *buffer, fs_offset_t filesize ) { return load( name, buffer, filesize ); }
qboolean Sound_LoadOggOpus( const char *name, const byte *buffer, fs_offset_t filesize ) { return false; }

// A stream keeps its whole file in memory (libmedia reads a file where it lies); pos is in bytes.
typedef struct
{
	struct media_audio *audio;
	byte *file;
} media_stream_t;

static stream_t *stream_open( const char *filename, int type )
{
	struct media_audio_info info;
	media_stream_t *ms;
	stream_t *stream;
	fs_offset_t size;
	byte *file;

	if( !( file = FS_LoadFile( filename, &size, false )))
		return NULL;
	ms = Mem_Calloc( host.soundpool, sizeof( *ms ));
	ms->file = file;
	if( !( ms->audio = media_audio_open( file, (uint32_t)size, NULL )))
	{
		Mem_Free( file );
		Mem_Free( ms );
		return NULL;
	}
	media_audio_get_info( ms->audio, &info );
	stream = Mem_Calloc( host.soundpool, sizeof( stream_t ));
	stream->channels = info.channels;
	stream->rate = info.rate;
	stream->width = 2;
	stream->type = type;
	stream->ptr = ms;
	return stream;
}

static int stream_read( stream_t *stream, int bytes, void *buffer )
{
	media_stream_t *ms = stream->ptr;
	int frame = stream->channels * 2, n;

	n = media_audio_read( ms->audio, buffer, bytes / frame );
	if( n <= 0 )
		return 0;
	stream->pos += n * frame;
	return n * frame;
}

static int stream_set_pos( stream_t *stream, int newpos )
{
	media_stream_t *ms = stream->ptr;
	int frame = stream->channels * 2;

	stream->pos = (size_t)media_audio_seek( ms->audio, newpos / frame ) * frame;
	return true;
}

static void stream_free( stream_t *stream )
{
	media_stream_t *ms = stream->ptr;

	if( ms )
	{
		media_audio_close( ms->audio );
		Mem_Free( ms->file );
		Mem_Free( ms );
	}
	Mem_Free( stream );
}

stream_t *Stream_OpenMPG( const char *filename ) { return stream_open( filename, WF_MPGDATA ); }
int Stream_ReadMPG( stream_t *stream, int bytes, void *buffer ) { return stream_read( stream, bytes, buffer ); }
int Stream_SetPosMPG( stream_t *stream, int newpos ) { return stream_set_pos( stream, newpos ); }
int Stream_GetPosMPG( stream_t *stream ) { return (int)stream->pos; }
void Stream_FreeMPG( stream_t *stream ) { stream_free( stream ); }

stream_t *Stream_OpenOggVorbis( const char *filename ) { return stream_open( filename, WF_VORBISDATA ); }
int Stream_ReadOggVorbis( stream_t *stream, int bytes, void *buffer ) { return stream_read( stream, bytes, buffer ); }
int Stream_SetPosOggVorbis( stream_t *stream, int newpos ) { return stream_set_pos( stream, newpos ); }
int Stream_GetPosOggVorbis( stream_t *stream ) { return (int)stream->pos; }
void Stream_FreeOggVorbis( stream_t *stream ) { stream_free( stream ); }

stream_t *Stream_OpenOggOpus( const char *filename ) { return NULL; }
int Stream_ReadOggOpus( stream_t *stream, int bytes, void *buffer ) { return 0; }
int Stream_SetPosOggOpus( stream_t *stream, int newpos ) { return false; }
int Stream_GetPosOggOpus( stream_t *stream ) { return 0; }
void Stream_FreeOggOpus( stream_t *stream ) { Mem_Free( stream ); }
