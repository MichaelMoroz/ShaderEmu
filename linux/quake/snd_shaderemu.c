/*
 * Quake's sound on the machine's sound card (docs/sound.md), in place of the game's mixer.
 * A sound is a voice pointed at its WAV where the pak file lies in the ROM: nothing is loaded
 * or mixed here. What stays the game's is which channel a sound takes and how loud it is in
 * each ear, worked out once a frame.
 */
#include "quakedef.h"
#include "../userland/shaderemu_sound.h"

#define MAX_SFX 512
#define DYNAMIC 8		/* sounds things make, as the game had */
#define AMBIENT 2		/* water and sky, by the leaf the viewer is in */
#define LOOPS 18		/* voices for a level's own sounds (torches, hums): the loudest there are */
#define MAX_STATIC 256
#define CLIP_DISTANCE 1000.0f
#define GAIN 0.5f		/* of the game's own level: its mix of a fight ran into the limit */

cvar_t bgmvolume = {"bgmvolume", "1", true};
cvar_t volume = {"volume", "0.7", true};
static cvar_t nosound = {"nosound", "0"};
static cvar_t ambient_level = {"ambient_level", "0.3"};
static cvar_t ambient_fade = {"ambient_fade", "100"};

unsigned VID_Milliseconds (void);

/* A sound's samples in the ROM. state 0: not looked at yet; 1: playable; 2: not. */
static struct { uint32_t address, length, rate, kind; int loop, state; } wav[MAX_SFX];
static sfx_t	*known_sfx;
static int	num_sfx;

static int	ready, pak_fd = -1;
static uint32_t	pak_at;			/* the pak file's first byte, as the card addresses it */
static int	voices[DYNAMIC + AMBIENT + LOOPS], voice_count;

typedef struct {
	sfx_t	*sfx;
	vec3_t	origin;
	float	master, dist_mult;
	int	entnum, entchannel, voice;	/* voice: an index into voices[], -1 for none */
	unsigned end_ms;
	int	loud;				/* left + right, as last worked out */
} channel;
static channel	dynamic[DYNAMIC], ambient[AMBIENT], statics[MAX_STATIC];
static int	static_count, loop_owner[LOOPS];	/* which static sound has each loop voice, -1 for none */
static sfx_t	*ambient_sfx[AMBIENT];
static vec3_t	ear, ear_right;

/* ---- the samples ---- */

static uint32_t word_at (const unsigned char *p) { return p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24; }

/* Finds a sound's samples, its rate and where it loops, from the WAV's chunks in the pak file. */
static int look (sfx_t *sfx)
{
	int		n = sfx - known_sfx, handle, size, width = 0, channels = 0;
	uint32_t	at, pos, data = 0, bytes = 0, mark = 0;
	unsigned char	b[40];
	char		name[MAX_QPATH + 8];

	if (wav[n].state)
		return wav[n].state == 1;
	wav[n].state = 2;
	wav[n].loop = -1;
	sprintf (name, "sound/%s", sfx->name);
	size = COM_OpenFile (name, &handle);
	if (size < 44)
		return 0;
	at = lseek (handle, 0, SEEK_CUR);
	if (handle != pak_fd) {		/* the pak's one handle: found once where it is in the ROM */
		pak_at = snd_rom (handle, "PACK", 4);
		pak_fd = pak_at ? handle : -1;
	}
	if (handle == pak_fd && pread (handle, b, 12, at) == 12 && !memcmp (b, "RIFF", 4) && !memcmp (b + 8, "WAVE", 4))
		for (pos = 12; pos + 8 <= (uint32_t)size; pos += 8 + ((word_at (b + 4) + 1) & ~1u)) {
			memset (b, 0, sizeof b);
			if (pread (handle, b, sizeof b, at + pos) < 8)
				break;
			if (!memcmp (b, "fmt ", 4))
				channels = b[10], wav[n].rate = word_at (b + 12), width = b[22] / 8;
			else if (!memcmp (b, "cue ", 4))
				wav[n].loop = word_at (b + 32);
			else if (!memcmp (b, "LIST", 4) && !memcmp (b + 28, "mark", 4))
				mark = word_at (b + 24);
			else if (!memcmp (b, "data", 4))
				data = at + pos + 8, bytes = word_at (b + 4);
		}
	COM_CloseFile (handle);
	if (!data || channels != 1 || (width != 1 && width != 2) || !wav[n].rate || (width == 2 && ((pak_at + data) & 1)))
		return 0;
	wav[n].address = pak_at + data;
	wav[n].length = bytes / width;
	if (wav[n].loop >= 0 && mark && wav[n].loop + mark < wav[n].length)
		wav[n].length = wav[n].loop + mark;
	if (wav[n].loop >= (int)wav[n].length)
		wav[n].loop = -1;
	wav[n].kind = (width == 1 ? SND_PCM8 : SND_PCM16) | SND_SMOOTH;
	wav[n].state = wav[n].length ? 1 : 2;
	return wav[n].state == 1;
}

sfx_t *S_PrecacheSound (char *name)
{
	int	i;

	if (!known_sfx || nosound.value || strlen (name) >= MAX_QPATH)
		return NULL;
	for (i = 0; i < num_sfx; i++)
		if (!strcmp (known_sfx[i].name, name))
			return &known_sfx[i];
	if (num_sfx == MAX_SFX)
		return NULL;
	strcpy (known_sfx[num_sfx].name, name);
	return &known_sfx[num_sfx++];
}

void S_TouchSound (char *name) {}
void S_ClearPrecache (void) {}
void S_BeginPrecaching (void) {}
void S_EndPrecaching (void) {}
void S_ClearBuffer (void) {}
void S_ExtraUpdate (void) {}
void S_AmbientOff (void) {}
void S_AmbientOn (void) {}

/* ---- how loud, in each ear ---- */

/* The card's volume word for a channel as the game's mixer had it: by distance, and by side. */
static uint32_t heard (channel *c)
{
	vec3_t	to;
	float	dist, dot, scale, left, right;
	int	l, r;

	if (c->entnum == cl.viewentity) {
		left = right = c->master * volume.value * GAIN;
	} else {
		VectorSubtract (c->origin, ear, to);
		dist = VectorNormalize (to) * c->dist_mult;
		dot = DotProduct (ear_right, to);
		scale = (1.0f - dist) * volume.value * GAIN;
		right = c->master * scale * (1.0f + dot);
		left = c->master * scale * (1.0f - dot);
	}
	l = left < 0 ? 0 : left > 256 ? 256 : (int)left;
	r = right < 0 ? 0 : right > 256 ? 256 : (int)right;
	c->loud = l + r;
	return SND_VOLUME (l, r);
}

static void start (channel *c, int voice, uint32_t vol)
{
	int	n = c->sfx - known_sfx;

	c->voice = voice;
	snd_play (voices[voice], wav[n].kind | (wav[n].loop >= 0 ? SND_LOOPED : 0), wav[n].address, wav[n].length,
		wav[n].loop >= 0 ? wav[n].loop : 0, snd_step (wav[n].rate), vol);
}

/* ---- the game's calls ---- */

void S_StartSound (int entnum, int entchannel, sfx_t *sfx, vec3_t origin, float fvol, float attenuation)
{
	unsigned	now = VID_Milliseconds ();
	int		i, pick = -1;
	channel		*c, probe;
	uint32_t	vol;

	if (!ready || !sfx || nosound.value || !look (sfx))
		return;
	/* the game's rule: a thing's channel is taken over; else whatever has least left to play, but never the player's by a monster */
	for (i = 0; i < DYNAMIC; i++) {
		c = &dynamic[i];
		if (entchannel != 0 && c->entnum == entnum && (c->entchannel == entchannel || entchannel == -1)) {
			pick = i;
			break;
		}
		if (c->sfx && (int)(c->end_ms - now) > 0 && c->entnum == cl.viewentity && entnum != cl.viewentity)
			continue;
		if (pick < 0 || (int)(c->end_ms - dynamic[pick].end_ms) < 0)
			pick = i;
	}
	if (pick < 0)
		return;
	memset (&probe, 0, sizeof probe);
	probe.sfx = sfx;
	VectorCopy (origin, probe.origin);
	probe.master = fvol * 255;
	probe.dist_mult = attenuation / CLIP_DISTANCE;
	probe.entnum = entnum, probe.entchannel = entchannel;
	vol = heard (&probe);
	if (!vol)
		return;		/* not heard from here */
	c = &dynamic[pick];
	*c = probe;
	c->end_ms = wav[sfx - known_sfx].loop >= 0 ? now + 0x3fffffff : now + wav[sfx - known_sfx].length * 1000 / wav[sfx - known_sfx].rate;
	start (c, pick, vol);
}

void S_StopSound (int entnum, int entchannel)
{
	int	i;

	for (i = 0; i < DYNAMIC && ready; i++)
		if (dynamic[i].sfx && dynamic[i].entnum == entnum && dynamic[i].entchannel == entchannel) {
			snd_stop (voices[i]);
			dynamic[i].sfx = NULL;
			dynamic[i].end_ms = VID_Milliseconds ();
		}
}

void S_StopAllSounds (qboolean clear)
{
	int	i;

	for (i = 0; i < voice_count; i++)
		snd_stop (voices[i]);
	memset (dynamic, 0, sizeof dynamic);
	memset (ambient, 0, sizeof ambient);
	static_count = 0;
	for (i = 0; i < LOOPS; i++)
		loop_owner[i] = -1;
}

void S_StaticSound (sfx_t *sfx, vec3_t origin, float vol, float attenuation)
{
	channel	*c;

	if (!ready || !sfx || static_count == MAX_STATIC || !look (sfx))
		return;
	if (wav[sfx - known_sfx].loop < 0) {
		Con_Printf ("Sound %s not looped\n", sfx->name);
		return;
	}
	c = &statics[static_count++];
	memset (c, 0, sizeof *c);
	c->sfx = sfx;
	VectorCopy (origin, c->origin);
	c->master = vol;
	c->dist_mult = (attenuation / 64) / CLIP_DISTANCE;
	c->voice = -1;
}

void S_LocalSound (char *name)
{
	sfx_t	*sfx = S_PrecacheSound (name);

	if (sfx)
		S_StartSound (cl.viewentity, -1, sfx, vec3_origin, 1, 1);
}

/* Water and sky: as loud as the viewer's leaf says, changing no faster than the game let it. */
static void ambients (void)
{
	extern mleaf_t	*r_viewleaf;
	int		i;

	for (i = 0; i < AMBIENT; i++) {
		channel	*c = &ambient[i];
		float	want = cl.worldmodel && r_viewleaf && cls.signon == SIGNONS ? ambient_level.value * r_viewleaf->ambient_sound_level[i] : 0;
		int	level;

		if (want < 8)
			want = 0;
		if (c->master < want)
			c->master = c->master + host_frametime * ambient_fade.value > want ? want : c->master + host_frametime * ambient_fade.value;
		else if (c->master > want)
			c->master = c->master - host_frametime * ambient_fade.value < want ? want : c->master - host_frametime * ambient_fade.value;
		level = (int)(c->master * volume.value * GAIN);
		level = level > 256 ? 256 : level;
		if (!ambient_sfx[i] || !look (ambient_sfx[i]))
			continue;
		if (level && !c->sfx) {
			c->sfx = ambient_sfx[i];
			start (c, DYNAMIC + i, SND_VOLUME (level, level));
		} else if (!level && c->sfx) {
			snd_stop (voices[DYNAMIC + i]);
			c->sfx = NULL;
		} else if (c->sfx) {
			SND_VOICE[voices[DYNAMIC + i]].volume = SND_VOLUME (level, level);
		}
	}
}

void S_Update (vec3_t origin, vec3_t forward, vec3_t right, vec3_t up)
{
	unsigned	now = VID_Milliseconds ();
	int		i, k;

	if (!ready)
		return;
	VectorCopy (origin, ear);
	VectorCopy (right, ear_right);
	ambients ();
	for (i = 0; i < DYNAMIC; i++)
		if (dynamic[i].sfx && (int)(dynamic[i].end_ms - now) > 0)
			SND_VOICE[voices[i]].volume = heard (&dynamic[i]);
	/* the level's own sounds: one that is heard has a voice, taken from a fainter one if need be */
	for (i = 0; i < static_count; i++) {
		channel		*c = &statics[i];
		uint32_t	vol = heard (c);

		if (c->voice >= 0) {
			if (vol) {
				SND_VOICE[voices[c->voice]].volume = vol;
			} else {
				snd_stop (voices[c->voice]);
				loop_owner[c->voice - DYNAMIC - AMBIENT] = -1;
				c->voice = -1;
			}
			continue;
		}
		if (!vol)
			continue;
		for (k = 0; k < LOOPS && loop_owner[k] >= 0; k++)
			;
		if (k == LOOPS) {
			int	faintest = 0;

			for (k = 1; k < LOOPS; k++)
				if (statics[loop_owner[k]].loud < statics[loop_owner[faintest]].loud)
					faintest = k;
			if (statics[loop_owner[faintest]].loud >= c->loud)
				continue;
			statics[loop_owner[faintest]].voice = -1;
			k = faintest;
		}
		if (DYNAMIC + AMBIENT + k >= voice_count)
			continue;
		loop_owner[k] = i;
		start (c, DYNAMIC + AMBIENT + k, vol);
	}
}

void S_Init (void)
{
	int	i;

	Cvar_RegisterVariable (&nosound);
	Cvar_RegisterVariable (&volume);
	Cvar_RegisterVariable (&bgmvolume);
	Cvar_RegisterVariable (&ambient_level);
	Cvar_RegisterVariable (&ambient_fade);
	known_sfx = Hunk_AllocName (MAX_SFX * sizeof (sfx_t), "sfx_t");
	for (i = 0; i < LOOPS; i++)
		loop_owner[i] = -1;
	if (COM_CheckParm ("-nosound") || snd_open ()) {
		Con_Printf ("No sound card\n");
		return;
	}
	while (voice_count < DYNAMIC + AMBIENT + LOOPS && (voices[voice_count] = snd_claim ()) >= 0)
		voice_count++;
	ready = voice_count >= DYNAMIC + AMBIENT;
	ambient_sfx[AMBIENT_WATER] = S_PrecacheSound ("ambience/water1.wav");
	ambient_sfx[AMBIENT_SKY] = S_PrecacheSound ("ambience/wind2.wav");
	Con_Printf ("Sound: %d voices of the machine's card\n", voice_count);
}

void S_Shutdown (void)
{
	if (ready)
		S_StopAllSounds (true);
}
