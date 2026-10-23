/*
gl_studio_job.c - a studio model's vertices made by a worker core (docs/games/halflife.md)

Included at the end of ref/gl's gl_studio.c. The first core sets each mesh's state and takes room
for its triangles in the frame; a job then moves the vertices by their bones, lights the normals
and writes the triangles there, as tagged vertices whose tag picks one of 256 shades.
*/
void hlglSay( const char *fmt, ... );	// (gl_shaderemu.c)
int hlglWorker( void );
void hlglPost( int k, uint ( *fn )( uint, uint ), uint a0, uint a1 );
uint *hlglShadedRoom( int count, const float *rgb, float alpha, uint **fill, uint *key );
void hlglShades( uint *table, uint key );
void *hlglJobMemory( unsigned bytes );
unsigned hlglWait( int k );
int hlglShadedTemplate( void *to, const float *rgb, float alpha, uint **fill, uint *key );
unsigned hlglTemplatePass( const void *t );
int hlglShadedRoomFor( int draws, unsigned vertices, unsigned passes, uint **commands, uint **room, uint *block, uint *first_mesh );
void hlglShadedPut( uint *c, const void *t, unsigned name, uint block, const void *at, uint first_mesh, int count );

#define JOB_VERTS 4096
#define JOB_MESHES 64
#define JOB_WORKERS 64

typedef struct studio_job_s
{
	const vec3_t	*verts, *norms;
	const byte	*vertbone, *normbone;
	int		numverts, numbones, nummesh;
	float		ambient, shade;
	const uint16_t	*gamma;		// NULL: linear
	const struct studio_job_s *whole;	// whose bones a part of a job reads (the whole job's own)
	const matrix3x4	*bonesp;	// the bones the whole job reads: its own copy, or the bones ahead's
	const matrix3x4	*blightfrom;	// the light's bones the job makes its bones' light vectors from (NULL: the whole job's blight)
	const vec3_t	*blightp;	// the light vectors it reads
	vec3_t		lightvec;
	int		blightnorm;
	// the draws' commands when the job writes them (else NULL): the states of the three kinds of mesh but for their textures
	uint		*commands, block, first_mesh, white;
	const mstudiotexture_t *ptexture;
	const short	*pskinref;
	uint		template[3][16];
	struct { const short *tricmds; uint *to, *table, key; int flags, numnorms; float s, t; int slot, first, corners, skin, kind; } mesh[JOB_MESHES];	// in the file's order
	matrix3x4	bones[MAXSTUDIOBONES];
	vec3_t		blight[MAXSTUDIOBONES];
	// chrome: the light's bones, made into each bone's two directions when a normal needs them
	int		chromed;
	vec3_t		chrome_origin, vright;
	matrix3x4	lightbones[MAXSTUDIOBONES];
	vec3_t		chromeright[MAXSTUDIOBONES], chromeup[MAXSTUDIOBONES];
	byte		chromemade[MAXSTUDIOBONES];
	vec3_t		xyz[JOB_VERTS];
	byte		tag[JOB_VERTS];
	float		chrome[JOB_VERTS][2];
} studio_job_t;

static studio_job_t *studio_jobs[JOB_WORKERS + 1];
static void R_StudioJobCopy( studio_job_t *to, const studio_job_t *from );

// How many triangles' corners the commands make.
static int R_StudioCorners( const short *ptricmds )
{
	int i, n = 0;

	while(( i = *( ptricmds++ )))
	{
		if( i < 0 ) i = -i;
		n += ( i - 2 ) * 3;
		ptricmds += 4 * i;
	}
	return n;
}

// The meshes' corners counted once a level (R_StudioJobLevel empties it, when models may move).
static struct { const short *tricmds; int corners; } corners_kept[1024];
void R_StudioJobLevel( void )
{
	memset( corners_kept, 0, sizeof( corners_kept ));
}
static int R_StudioCornersKept( const short *ptricmds )
{
	int k = (uintptr_t)ptricmds >> 3 & 1023;

	if( corners_kept[k].tricmds != ptricmds )
		corners_kept[k].tricmds = ptricmds, corners_kept[k].corners = R_StudioCorners( ptricmds );
	return corners_kept[k].corners;
}

static byte R_StudioShadeOf( const studio_job_t *job, int flags, int bone, const float *normal )
{
	float illum = job->ambient;

	if( FBitSet( flags, STUDIO_NF_FULLBRIGHT ))
		return 255;
	if( FBitSet( flags, STUDIO_NF_FLATSHADE ))
		illum += job->shade * 0.8f;
	else
	{
		const float r = SHADE_LAMBERT;	// > 1: R_StudioLighting's second case
		float lightcos = Q_min( DotProduct( normal, job->blightp[bone] ), 1.0f );

		illum += job->shade;
		lightcos = ( lightcos + ( r - 1.0f )) / r;
		if( lightcos > 0.0f )
			illum -= job->shade * lightcos;
		illum = Q_max( illum, 0.0f );
	}
	illum = Q_min( illum, 255.0f );

	uint b = (uint)( illum * 4 );
	if( b >= 1024 ) b = 0;
	if( job->gamma ) b = job->gamma[b];
	return (byte)(( b * 255 + 511 ) / 1023 );
}

// R_StudioSetupChrome's
static void R_StudioChromeOf( studio_job_t *job, int bone, const float *normal, float *pchrome )
{
	if( !job->chromemade[bone] )
	{
		vec3_t up, right, tmp;
		const studio_job_t *w = job->whole;
		VectorNegate( w->chrome_origin, tmp );
		tmp[0] += w->lightbones[bone][0][3];
		tmp[1] += w->lightbones[bone][1][3];
		tmp[2] += w->lightbones[bone][2][3];
		VectorNormalize( tmp );
		CrossProduct( tmp, w->vright, up );
		VectorNormalize( up );
		CrossProduct( up, tmp, right );
		VectorNormalize( right );
		Matrix3x4_VectorIRotate( w->lightbones[bone], up, job->chromeup[bone] );
		Matrix3x4_VectorIRotate( w->lightbones[bone], right, job->chromeright[bone] );
		job->chromemade[bone] = 1;
	}
	pchrome[0] = ( DotProduct( normal, job->chromeright[bone] ) + 1.0f ) * 32.0f;
	pchrome[1] = ( DotProduct( normal, job->chromeup[bone] ) + 1.0f ) * 32.0f;
}

static inline void R_StudioCorner( uint *to, const studio_job_t *job, const short *c, float s, float t, qboolean chrome )
{
	const float *v = job->xyz[c[0]];
	float cs = chrome ? job->chrome[c[1]][0] : c[2], ct = chrome ? job->chrome[c[1]][1] : c[3];
	uint u = (uint)(int)( cs * s * 1024.0f ) & 0xfff, w = (uint)(int)( ct * t * 1024.0f ) & 0xfff;

	memcpy( to, v, 12 );
	to[3] = u | w << 12 | (uint)job->tag[c[1]] << 24;
}

// a1: the meshes it draws, from a1 & 255 to a1 >> 8 & 255 (all the vertices are moved for any of them);
// a part's worker in a1 >> 16, whose job is made a copy of the whole a0 first
static uint R_StudioJob( uint a0, uint a1 )
{
	studio_job_t *job = (studio_job_t *)a0;
	int k = 0, lo = a1 & 255, hi = a1 >> 8 & 255;

	if( a1 >> 16 )
		R_StudioJobCopy( studio_jobs[a1 >> 16], job ), job = studio_jobs[a1 >> 16];

	for( int i = 0; i < job->numverts; i++ )
		Matrix3x4_VectorTransform( job->whole->bonesp[job->vertbone[i]], job->verts[i], job->xyz[i] );
	for( int b = 0; job->blightfrom && b < job->numbones; b++ )	// (R_StudioSetupLighting's)
	{
		Matrix3x4_VectorIRotate( job->blightfrom[b], job->lightvec, job->blight[b] );
		if( job->blightnorm ) VectorNormalize( job->blight[b] );
	}
	job->blightp = job->blightfrom ? job->blight : job->whole->blight;
	if( job->chromed )
		memset( job->chromemade, 0, job->numbones );
	for( int j = 0; j < lo; j++ )
		k += job->mesh[j].numnorms;
	for( int j = lo; j < hi; j++ )
		for( int i = 0; i < job->mesh[j].numnorms; i++, k++ )
		{
			job->tag[k] = R_StudioShadeOf( job, job->mesh[j].flags, job->normbone[k], job->norms[k] );
			if( FBitSet( job->mesh[j].flags, STUDIO_NF_CHROME ))
				R_StudioChromeOf( job, job->normbone[k], job->norms[k], job->chrome[k] );
		}

	for( int j = lo; j < hi && job->commands; j++ )
	{
		uint n = job->white ? job->white : job->ptexture[job->pskinref[job->mesh[j].skin]].index;
		if( n <= 0 || n >= MAX_TEXTURES ) n = tr.defaultTexture;
		hlglShadedPut( job->commands + 16 * job->mesh[j].slot, job->template[job->mesh[j].kind], R_GetTexture( n )->texnum,
			job->block, job->mesh[j].to, job->first_mesh + job->mesh[j].first, job->mesh[j].corners );
	}
	for( int j = lo; j < hi; j++ )
	{
		const short *ptricmds = job->mesh[j].tricmds;
		if( job->mesh[j].table )
			hlglShades( job->mesh[j].table, job->mesh[j].key );
		uint *to = job->mesh[j].to, a[4], b[4];
		float s = job->mesh[j].s, t = job->mesh[j].t;
		qboolean chrome = FBitSet( job->mesh[j].flags, STUDIO_NF_CHROME ) != 0;
		int i;

		if( !to )
			continue;
		while(( i = *( ptricmds++ )))
		{
			qboolean fan = i < 0;
			if( fan ) i = -i;
			R_StudioCorner( a, job, ptricmds, s, t, chrome ), ptricmds += 4;
			R_StudioCorner( b, job, ptricmds, s, t, chrome ), ptricmds += 4;
			for( int n = 0; n < i - 2; n++, ptricmds += 4, to += 12 )
			{
				uint c[4];
				R_StudioCorner( c, job, ptricmds, s, t, chrome );
				// as triangles_of in gl_shaderemu.c: a strip's odd ones turned round
				if( fan ) memcpy( to, a, 16 ), memcpy( to + 4, b, 16 ), memcpy( to + 8, c, 16 ), memcpy( b, c, 16 );
				else if( n & 1 ) memcpy( to, b, 16 ), memcpy( to + 4, a, 16 ), memcpy( to + 8, c, 16 ), memcpy( a, b, 16 ), memcpy( b, c, 16 );
				else memcpy( to, a, 16 ), memcpy( to + 4, b, 16 ), memcpy( to + 8, c, 16 ), memcpy( a, b, 16 ), memcpy( b, c, 16 );
			}
		}
	}
	return 0;
}

// What a part of a job reads, copied from the whole: the bones it reads where they are.
static void R_StudioJobCopy( studio_job_t *to, const studio_job_t *from )
{
	memcpy( to, from, offsetof( studio_job_t, mesh ));
	memcpy( to->mesh, from->mesh, sizeof( from->mesh[0] ) * from->nummesh );
	to->chromed = from->chromed;
}

// A job of more corners and normals than this is shared by meshes among up to JOB_PARTS workers.
#define JOB_PART 1200
#define JOB_PARTS 3
static int parts_of[JOB_WORKERS + 1][JOB_PARTS];	// the workers whose parts read a worker's job

// The meshes' commands and vertices taken for the job to write, in the order given (true): the states
// of the kinds of mesh made here, each as the loop of R_StudioPointsOnWorker sets it.
static qboolean R_StudioCommandsOnWorker( studio_job_t *job, cl_entity_t *e, const int *order, mstudiotexture_t *ptexture, short *pskinref, mstudiomesh_t *pmesh )
{
	static int off = -1;
	uint corners = 0, passes = 0;
	int kinds = 0;

	if( off < 0 ) off = getenv( "HL_STUDIO_COMMANDS" ) && !strcmp( getenv( "HL_STUDIO_COMMANDS" ), "inline" );
	job->commands = NULL;
	if( off || FBitSet( g_nForceFaceFlags, STUDIO_NF_CHROME ))
		return false;
	for( int n = 0; n < job->nummesh; n++ )
	{
		int j = order[n], f = job->mesh[j].flags;
		job->mesh[j].kind = FBitSet( f, STUDIO_NF_MASKED ) ? 1 : FBitSet( f, STUDIO_NF_ADDITIVE ) ? 2 : 0;
		job->mesh[j].corners = R_StudioCornersKept( job->mesh[j].tricmds );
		job->mesh[j].slot = n, job->mesh[j].first = corners, job->mesh[j].skin = pmesh[j].skinref;
		job->mesh[j].table = NULL;
		corners += job->mesh[j].corners;
		kinds |= 1 << job->mesh[j].kind;
	}
	for( int k = 0; k < 3; k++ )
	{
		uint *fill = NULL, key = 0;
		float oldblend = tr.blend;
		int made;

		if( !( kinds & ( 1 << k )))
			continue;
		if( k == 1 )
		{
			pglEnable( GL_ALPHA_TEST );
			pglAlphaFunc( GL_GREATER, 0.5f );
			pglDepthMask( GL_TRUE );
			if( R_ModelOpaque( e->curstate.rendermode ))
				tr.blend = 1.0f;
		}
		else if( k == 2 )
		{
			if( R_ModelOpaque( e->curstate.rendermode ))
			{
				pglBlendFunc( GL_ONE, GL_ONE );
				pglDepthMask( GL_FALSE );
				pglEnable( GL_BLEND );
				R_AllowFog( false );
			}
			else pglBlendFunc( GL_SRC_ALPHA, GL_ONE );
		}
		made = hlglShadedTemplate( job->template[k], g_studio.lightcolor, tr.blend, &fill, &key );
		if( k == 1 )
		{
			pglAlphaFunc( GL_GREATER, DEFAULT_ALPHATEST );
			pglDisable( GL_ALPHA_TEST );
		}
		else if( k == 2 && R_ModelOpaque( e->curstate.rendermode ))
		{
			pglDepthMask( GL_TRUE );
			pglDisable( GL_BLEND );
			R_AllowFog( true );
		}
		tr.blend = oldblend;
		if( !made )
			return false;
		passes |= 1u << hlglTemplatePass( job->template[k] );
		for( int n = 0; n < job->nummesh && fill; n++ )	// (the first mesh of the kind fills its table)
			if( job->mesh[order[n]].kind == k )
			{
				job->mesh[order[n]].table = fill, job->mesh[order[n]].key = key;
				break;
			}
	}
	uint *room;
	if( !corners || !hlglShadedRoomFor( job->nummesh, corners, passes, &job->commands, &room, &job->block, &job->first_mesh ))
	{
		job->commands = NULL;
		return false;
	}
	for( int j = 0; j < job->nummesh; j++ )
	{
		job->mesh[j].to = room + 4 * job->mesh[j].first;
		r_stats.c_studio_polys += pmesh[j].numtris;
	}
	job->ptexture = m_fDoRemap ? gEngfuncs.CL_GetRemapInfoForEntity( e )->ptexture : NULL;	// (R_StudioSetupSkin's)
	if( !job->ptexture ) job->ptexture = ptexture;
	job->pskinref = pskinref;
	job->white = r_lightmap->value && !r_fullbright->value ? tr.whiteTexture : 0;
	return true;
}

// The submodel drawn by a job (true), or not one a job can draw.
static qboolean R_StudioPointsOnWorker( void )
{
	static int off = -1;
	cl_entity_t *e = RI.currententity;

	static int logged;
	const char *why = NULL;

	if( off < 0 ) off = getenv( "HL_STUDIO" ) ? !strcmp( getenv( "HL_STUDIO" ), "inline" ) ? 1 : 2 : 0;
	if( off == 1 || !m_pStudioHeader || !e )
		return false;
	if( g_studio.numlocallights ) why = "local lights";
	else if( e->curstate.renderfx == kRenderFxGlowShell ) why = "glow shell";
	else if( e->curstate.rendermode == kRenderTransAdd ) why = "added";
	else if( FBitSet( g_nForceFaceFlags, STUDIO_NF_CHROME )) why = "chrome";
	else if( FBitSet( m_pStudioHeader->flags, STUDIO_HAS_BONEWEIGHTS ) && m_pSubModel->blendvertinfoindex ) why = "bone weights";
	else if( m_pSubModel->numverts > JOB_VERTS || m_pSubModel->numnorms > JOB_VERTS || m_pSubModel->nummesh > JOB_MESHES
		|| m_pStudioHeader->numbones > MAXSTUDIOBONES ) why = "too big";
	if( why && off == 2 && logged++ < 64 )   // (HL_STUDIO=log)
		hlglSay( "studio inline: %s %s\n", m_pStudioHeader->name, why );
	if( why )
		return false;

	int m_skinnum = e->curstate.skin;
	mstudiotexture_t *ptexture = (mstudiotexture_t *)((byte *)m_pStudioHeader + m_pStudioHeader->textureindex);
	mstudiomesh_t *pmesh = (mstudiomesh_t *)((byte *)m_pStudioHeader + m_pSubModel->meshindex);
	short *pskinref = (short *)((byte *)m_pStudioHeader + m_pStudioHeader->skinindex);
	if( m_skinnum > 0 && m_skinnum < m_pStudioHeader->numskinfamilies )
		pskinref += (m_skinnum * m_pStudioHeader->numskinref);
	for( int j = 0; j < m_pSubModel->nummesh; j++ )
		if( FBitSet( ptexture[pskinref[pmesh[j].skinref]].flags | g_nForceFaceFlags, STUDIO_NF_UV_COORDS ))
		{
			if( off == 2 && logged++ < 64 )
				hlglSay( "studio inline: %s float mesh\n", m_pStudioHeader->name );
			return false;
		}

	int k = hlglWorker();
	if( k < 0 || k > JOB_WORKERS )
		return false;
	if( !studio_jobs[k] && !( studio_jobs[k] = hlglJobMemory( sizeof( studio_job_t ))))
		return false;
	studio_job_t *job = studio_jobs[k];
	for( int p = 0; p < JOB_PARTS; p++ )	// (their parts may still read the bones about to be written)
		if( parts_of[k][p] )
			hlglWait( parts_of[k][p] ), parts_of[k][p] = 0;
	job->whole = job;

	job->verts = (vec3_t *)((byte *)m_pStudioHeader + m_pSubModel->vertindex);
	job->norms = (vec3_t *)((byte *)m_pStudioHeader + m_pSubModel->normindex);
	job->vertbone = (byte *)m_pStudioHeader + m_pSubModel->vertinfoindex;
	job->normbone = (byte *)m_pStudioHeader + m_pSubModel->norminfoindex;
	job->numverts = m_pSubModel->numverts;
	job->numbones = m_pStudioHeader->numbones;
	job->nummesh = m_pSubModel->nummesh;
	job->ambient = g_studio.ambientlight, job->shade = g_studio.shadelight;
	job->gamma = FBitSet( gp_host->features, ENGINE_LINEAR_GAMMA_SPACE ) ? NULL : tr.lightgammatable;
	const float *ahead = SE_BonesAhead();
	if( ahead )
		job->bonesp = (const matrix3x4 *)ahead;
	else memcpy( job->bones, g_studio.bonestransform, sizeof( matrix3x4 ) * job->numbones ), job->bonesp = job->bones;
	job->blightfrom = NULL;
	if( se_blight_lazy && ahead )
	{
		job->blightfrom = (const matrix3x4 *)( ahead + 12 * job->numbones );
		VectorCopy( g_studio.lightvec, job->lightvec ), job->blightnorm = se_blight_scale > 1.0f;
	}
	else
	{
		if( se_blight_lazy )
			SE_BlightNow();
		memcpy( job->blight, g_studio.blightvec, sizeof( vec3_t ) * job->numbones );
	}
	job->chromed = 0;

	int order[JOB_MESHES];
	for( int j = 0; j < job->nummesh; j++ )
	{
		job->mesh[j].tricmds = (short *)((byte *)m_pStudioHeader + pmesh[j].triindex);
		job->mesh[j].flags = ptexture[pskinref[pmesh[j].skinref]].flags | g_nForceFaceFlags;
		job->mesh[j].numnorms = pmesh[j].numnorms;
		job->mesh[j].s = 1.0f / (float)ptexture[pskinref[pmesh[j].skinref]].width;
		job->mesh[j].t = 1.0f / (float)ptexture[pskinref[pmesh[j].skinref]].height;
		job->mesh[j].to = job->mesh[j].table = NULL;
		order[j] = j;
		if( FBitSet( job->mesh[j].flags, STUDIO_NF_CHROME ) && !job->chromed )
		{
			job->chromed = 1;
			VectorCopy( g_studio.chrome_origin, job->chrome_origin );
			VectorCopy( RI.vright, job->vright );
			memcpy( job->lightbones, g_studio.lighttransform, sizeof( matrix3x4 ) * job->numbones );
		}
	}
	// R_StudioMeshCompare's order: masked first, additive last
	if( r_studio_sort_textures.value )
	{
		int n = 0;
		for( int pass = 0; pass < 3; pass++ )
			for( int j = 0; j < job->nummesh; j++ )
			{
				int f = job->mesh[j].flags, p = FBitSet( f, STUDIO_NF_ADDITIVE ) ? 2 : FBitSet( f, STUDIO_NF_MASKED ) ? 0 : 1;
				if( p == pass ) order[n++] = j;
			}
	}

	if( glState.faceCull != GL_NONE )
	{
		tr.fFlipViewModel = R_AllowFlipViewModel( e );
		GL_Cull( tr.fFlipViewModel ? GL_NONE : GL_FRONT );
	}

	if( R_StudioCommandsOnWorker( job, e, order, ptexture, pskinref, pmesh ))
		goto posted;
	for( int n = 0; n < job->nummesh; n++ )
	{
		int j = order[n];
		float oldblend = tr.blend;

		g_nFaceFlags = job->mesh[j].flags;
		if( FBitSet( g_nFaceFlags, STUDIO_NF_MASKED ))
		{
			pglEnable( GL_ALPHA_TEST );
			pglAlphaFunc( GL_GREATER, 0.5f );
			pglDepthMask( GL_TRUE );
			if( R_ModelOpaque( e->curstate.rendermode ))
				tr.blend = 1.0f;
		}
		else if( FBitSet( g_nFaceFlags, STUDIO_NF_ADDITIVE ))
		{
			if( R_ModelOpaque( e->curstate.rendermode ))
			{
				pglBlendFunc( GL_ONE, GL_ONE );
				pglDepthMask( GL_FALSE );
				pglEnable( GL_BLEND );
				R_AllowFog( false );
			}
			else pglBlendFunc( GL_SRC_ALPHA, GL_ONE );
		}

		R_StudioSetupSkin( m_pStudioHeader, pskinref[pmesh[j].skinref] );
		job->mesh[j].to = hlglShadedRoom( R_StudioCornersKept( job->mesh[j].tricmds ), g_studio.lightcolor, tr.blend, &job->mesh[j].table, &job->mesh[j].key );

		if( FBitSet( g_nFaceFlags, STUDIO_NF_MASKED ))
		{
			pglAlphaFunc( GL_GREATER, DEFAULT_ALPHATEST );
			pglDisable( GL_ALPHA_TEST );
		}
		else if( FBitSet( g_nFaceFlags, STUDIO_NF_ADDITIVE ) && R_ModelOpaque( e->curstate.rendermode ))
		{
			pglDepthMask( GL_TRUE );
			pglDisable( GL_BLEND );
			R_AllowFog( true );
		}
		r_stats.c_studio_polys += pmesh[j].numtris;
		tr.blend = oldblend;
	}
	if(( tr.framecount & 255 ) == 7 )	// DEBUG
		hlglSay( "studio job %d: %s verts %d norms %d meshes %d\n", k, m_pStudioHeader->name, job->numverts, m_pSubModel->numnorms, job->nummesh );
posted:;
	int cost[JOB_MESHES], total = 0;
	for( int j = 0; j < job->nummesh; j++ )
		total += cost[j] = ( job->mesh[j].to ? R_StudioCornersKept( job->mesh[j].tricmds ) : 0 ) + job->mesh[j].numnorms;
	int parts = Q_min( total / JOB_PART + 1, JOB_PARTS ), lo = 0, done = 0, pk[JOB_PARTS], range[JOB_PARTS];
	studio_job_t *pj[JOB_PARTS];
	for( int p = 0; p < parts; p++ )
	{
		int hi = lo;
		while( hi < job->nummesh && ( p == parts - 1 || done + cost[hi] / 2 < total * ( p + 1 ) / parts ))
			done += cost[hi++];
		pk[p] = k, pj[p] = job, range[p] = lo | hi << 8;
		if( p > 0 && ( pk[p] = hlglWorker( )) > 0 && pk[p] <= JOB_WORKERS && pk[p] != k
			&& ( studio_jobs[pk[p]] || ( studio_jobs[pk[p]] = hlglJobMemory( sizeof( studio_job_t )))))
		{
			for( int q = 0; q < JOB_PARTS; q++ )	// (the parts of its own job last read it)
				if( parts_of[pk[p]][q] )
					hlglWait( parts_of[pk[p]][q] ), parts_of[pk[p]][q] = 0;
			range[p] |= pk[p] << 16;
		}
		else if( p > 0 )
		{
			range[p - 1] = ( range[p - 1] & ~0xff00u ) | job->nummesh << 8, parts = p;	// (no worker for it: the part before's)
			break;
		}
		lo = hi;
	}
	for( int p = 0; p < parts; p++ )
		hlglPost( pk[p], R_StudioJob, (uint)pj[p], range[p] ), parts_of[k][p] = p ? pk[p] : 0;
	return true;
}
