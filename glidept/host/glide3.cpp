//**************************************************************
//* Glide 3 on OpenGLide (2ksbox, docs/tracks/m14-glide3.md).
//*
//* qemu-3dfx's dispatcher serves a guest's glide3x.dll from the same
//* host library as its glide2x.dll: for every entry point it looks up
//* `wrap3x_<name>` first and `<name>` after (hw/3dfx/glide2x_impl.c,
//* init_glide2x). That settles the shape of this file. Everything Glide 3
//* kept from Glide 2 with the same meaning is OpenGLide's own export,
//* untouched. What Glide 3 re-encoded under an old name is a wrap3x_
//* export here, translating into OpenGLide's Glide 2 calls: the vertex
//* (the game's own layout instead of GrVertex), the texture level of detail
//* and aspect ratio (log2, reversed), the texture tables (no TMU argument),
//* grSstWinOpen's context and grLfbWriteRegion's pixel-pipeline argument.
//* What Glide 3 added is a plain export: grVertexLayout, the vertex
//* arrays, grGet and grGetString, the coordinate spaces.
//*
//* One library therefore serves both APIs, which is why build-glide.sh
//* only links libglide3x as another name for libglide2x, and why the
//* player's QEMU_GLIDE_LIB, which names the one file, is right for both.
//*
//* Written from 3dfx's released Glide 3 source as the specification
//* (glidept/glide3.h says which tree); none of its text is here.
//*
//* SPDX-License-Identifier: LGPL-2.1-or-later (matches OpenGLide)
//**************************************************************
#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <string.h>

#include "GlOgl.h"
#include "GLRender.h"

#include "glide3.h"
#include "host.h"

// OpenGLide's Glide 2 entry points this layer calls, with the signatures
// sdk2_glide.h gives them
FX_ENTRY void FX_CALL grHints( GrHint_t hintType, FxU32 hintMask );

namespace {

// grVertexLayout's table: a byte offset per parameter, -1 when disabled.
// Sixteen bits each, because of the size limit below; hw/3dfx also gives
// a vertex-layout save area only SIZE_GRVERTEX (60) bytes unless the game
// asked grGet for the size first.
struct Layout
{
    FxI16 xy, z, w, q, fog, a, rgb, pargb, st0, st1, q0, q1;
};

// Everything Glide 3 added to the state, saved and restored with
// grGlideGetState / grGlideSetState after OpenGLide's own GlideState.
struct G3State
{
    Layout  layout;
    FxU16   coords;         // GR_WINDOW_COORDS or GR_CLIP_COORDS
    FxU16   enables;        // bit n = grEnable(n)
    FxI16   vp[ 4 ];        // grViewport: x, y, width, height
    float   depth[ 2 ];     // grDepthRange: near, far
};

// hw/3dfx's default grGlideGetState save area is SIZE_GRSTATE (312) bytes,
// for a game that never asks grGet(GR_GLIDE_STATE_SIZE): both halves must
// fit in it or grGlideSetState would read past the end. OpenGLide's own
// GlideState is 240 of them.
static_assert( sizeof( GlideState ) + sizeof( G3State ) <= 312,
               "Glide 3 state no longer fits hw/3dfx's default GrState" );
static_assert( sizeof( Layout ) <= 60,
               "the vertex layout no longer fits hw/3dfx's default save area" );

G3State g3;

// Per TMU, from the source texture's aspect ratio: what a clip-space s or
// t of 1.0 is in texels. Not saved with the state, like the texture source
// it follows (OpenGLide's grGlideSetState does not restore that either).
float sscale[ 2 ], tscale[ 2 ];

// GR_TRIANGLE_STRIP_CONTINUE / GR_TRIANGLE_FAN_CONTINUE pick up where the
// last strip or fan left off. The guest's vertices live in hw/3dfx's shared
// memory, which the next call overwrites, so what is kept is our copy.
struct
{
    GrVertex a, b;      // strip: the last two; fan: the pivot and the last
    bool     odd;       // strip: the next triangle's winding is reversed
    int      have;      // how many of a, b are valid (0..2)
} strip, fan;

void reset_layout( void )
{
    FxI16 *p = &g3.layout.xy;
    for ( size_t i = 0; i < sizeof( Layout ) / sizeof( FxI16 ); i++ )
    {
        p[ i ] = -1;
    }
}

void reset_state( void )
{
    memset( &g3, 0, sizeof( g3 ) );
    reset_layout( );
    g3.coords = GR_WINDOW_COORDS;
    g3.vp[ 2 ] = 640;
    g3.vp[ 3 ] = 480;
    g3.depth[ 1 ] = 1.0f;
    sscale[ 0 ] = sscale[ 1 ] = 256.0f;
    tscale[ 0 ] = tscale[ 1 ] = 256.0f;
    strip.have = fan.have = 0;
}

// Glide 3 has no grHints: every TMU has its own 1/w (GR_PARAM_Q0/Q1, or
// in clip space the one the divide made), so this layer always fills
// tmuvtx[].oow and tells OpenGLide's triangle setup to use it.
// grSstWinOpen resets the hint, so it is applied again there.
void apply_hints( void )
{
    grHints( GR_HINT_STWHINT, GR_STWHINT_W_DIFF_TMU0 | GR_STWHINT_W_DIFF_TMU1 );
}

inline float fetch( const FxU8 *v, FxI32 off )
{
    float f;
    memcpy( &f, v + off, sizeof( f ) );
    return f;
}

// One game vertex, described by the layout, as the GrVertex OpenGLide's
// Glide 2 setup consumes. Window coordinates are Glide 2's own units, so
// they copy across. Clip coordinates are 3dfx's rules: divide by w, map
// through the viewport and the depth range, colours from [0,1] to
// [0,255], s and t from [0,1] to the source texture's texel scale.
void to_vertex( const void *p, GrVertex *out )
{
    const FxU8   *v = (const FxU8 *)p;
    const Layout &L = g3.layout;
    float        cscale = 1.0f;

    memset( out, 0, sizeof( *out ) );
    float x = L.xy >= 0 ? fetch( v, L.xy ) : 0.0f;
    float y = L.xy >= 0 ? fetch( v, L.xy + 4 ) : 0.0f;

    if ( g3.coords == GR_CLIP_COORDS )
    {
        float w = L.w >= 0 ? fetch( v, L.w ) : 1.0f;
        float oow = w != 0.0f ? 1.0f / w : 1.0f;
        float hw = g3.vp[ 2 ] * 0.5f, hh = g3.vp[ 3 ] * 0.5f;
        float hdepth = ( g3.depth[ 1 ] - g3.depth[ 0 ] ) * 0.5f * 65535.0f;
        float oz = ( g3.depth[ 1 ] + g3.depth[ 0 ] ) * 0.5f * 65535.0f;

        out->x = x * oow * hw + ( g3.vp[ 0 ] + hw );
        out->y = y * oow * hh + ( g3.vp[ 1 ] + hh );
        out->ooz = L.z >= 0 ? fetch( v, L.z ) * oow * hdepth + oz : 0.0f;
        out->oow = L.q >= 0 ? fetch( v, L.q ) * oow : oow;
        out->tmuvtx[ 0 ].oow = L.q0 >= 0 ? fetch( v, L.q0 ) * oow : oow;
        out->tmuvtx[ 1 ].oow = L.q1 >= 0 ? fetch( v, L.q1 ) * oow : oow;
        if ( L.st0 >= 0 )
        {
            out->tmuvtx[ 0 ].sow = fetch( v, L.st0 ) * oow * sscale[ 0 ];
            out->tmuvtx[ 0 ].tow = fetch( v, L.st0 + 4 ) * oow * tscale[ 0 ];
        }
        if ( L.st1 >= 0 )
        {
            out->tmuvtx[ 1 ].sow = fetch( v, L.st1 ) * oow * sscale[ 1 ];
            out->tmuvtx[ 1 ].tow = fetch( v, L.st1 + 4 ) * oow * tscale[ 1 ];
        }
        cscale = 255.0f;
    }
    else
    {
        out->x = x;
        out->y = y;
        out->ooz = L.z >= 0 ? fetch( v, L.z ) : 0.0f;
        out->oow = L.q >= 0 ? fetch( v, L.q ) : 1.0f;
        out->tmuvtx[ 0 ].oow = L.q0 >= 0 ? fetch( v, L.q0 ) : out->oow;
        out->tmuvtx[ 1 ].oow = L.q1 >= 0 ? fetch( v, L.q1 ) : out->oow;
        if ( L.st0 >= 0 )
        {
            out->tmuvtx[ 0 ].sow = fetch( v, L.st0 );
            out->tmuvtx[ 0 ].tow = fetch( v, L.st0 + 4 );
        }
        if ( L.st1 >= 0 )
        {
            out->tmuvtx[ 1 ].sow = fetch( v, L.st1 );
            out->tmuvtx[ 1 ].tow = fetch( v, L.st1 + 4 );
        }
    }

    if ( L.pargb >= 0 )
    {
        // packed ARGB, one byte each whatever the coordinate space
        FxU32 c;
        memcpy( &c, v + L.pargb, sizeof( c ) );
        out->a = (float)( ( c >> 24 ) & 0xff );
        out->r = (float)( ( c >> 16 ) & 0xff );
        out->g = (float)( ( c >> 8 ) & 0xff );
        out->b = (float)( c & 0xff );
    }
    else
    {
        if ( L.rgb >= 0 )
        {
            out->r = fetch( v, L.rgb ) * cscale;
            out->g = fetch( v, L.rgb + 4 ) * cscale;
            out->b = fetch( v, L.rgb + 8 ) * cscale;
        }
        if ( L.a >= 0 )
        {
            out->a = fetch( v, L.a ) * cscale;
        }
    }
}

// what grDrawTriangle does after RenderAddTriangle, once per call
void flush_front( void )
{
    if ( Glide.State.RenderBuffer == GR_BUFFER_FRONTBUFFER )
    {
        RenderDrawTriangles( );
        glFlush( );
    }
}

void strip_add( const GrVertex &c )
{
    if ( strip.have < 2 )
    {
        ( strip.have == 0 ? strip.a : strip.b ) = c;
        strip.have++;
        return;
    }
    // every other triangle of a strip is wound the other way; swapping its
    // first two vertices keeps the whole strip facing one way for culling
    if ( strip.odd )
    {
        RenderAddTriangle( &strip.b, &strip.a, &c, true );
    }
    else
    {
        RenderAddTriangle( &strip.a, &strip.b, &c, true );
    }
    strip.a = strip.b;
    strip.b = c;
    strip.odd = !strip.odd;
}

void fan_add( const GrVertex &c )
{
    if ( fan.have < 2 )
    {
        ( fan.have == 0 ? fan.a : fan.b ) = c;
        fan.have++;
        return;
    }
    RenderAddTriangle( &fan.a, &fan.b, &c, true );
    fan.b = c;
}

// grDrawVertexArray and grDrawVertexArrayContiguous differ only in how the
// i-th vertex is found
template < class At >
void draw_array( FxU32 mode, FxU32 count, At at )
{
    GrVertex a, b, c;

    switch ( mode )
    {
    case GR_POINTS:
        for ( FxU32 i = 0; i < count; i++ )
        {
            to_vertex( at( i ), &a );
            RenderAddPoint( &a, true );
        }
        break;

    case GR_LINE_STRIP:
    case GR_LINES:
        // RenderAddLine draws at once; what is queued goes first
        RenderDrawTriangles( );
        for ( FxU32 i = 0; i + 1 < count; i += ( mode == GR_LINES ) ? 2 : 1 )
        {
            to_vertex( at( i ), &a );
            to_vertex( at( i + 1 ), &b );
            RenderAddLine( &a, &b, true );
        }
        break;

    case GR_TRIANGLES:
        for ( FxU32 i = 0; i + 2 < count; i += 3 )
        {
            to_vertex( at( i ), &a );
            to_vertex( at( i + 1 ), &b );
            to_vertex( at( i + 2 ), &c );
            RenderAddTriangle( &a, &b, &c, true );
        }
        break;

    case GR_TRIANGLE_STRIP:
        strip.have = 0;
        strip.odd = false;
        // fall through: a new strip is a continued empty one
    case GR_TRIANGLE_STRIP_CONTINUE:
        for ( FxU32 i = 0; i < count; i++ )
        {
            to_vertex( at( i ), &c );
            strip_add( c );
        }
        break;

    case GR_POLYGON:
    case GR_TRIANGLE_FAN:
        fan.have = 0;
        // fall through
    case GR_TRIANGLE_FAN_CONTINUE:
        for ( FxU32 i = 0; i < count; i++ )
        {
            to_vertex( at( i ), &c );
            fan_add( c );
        }
        break;

    default:
        GlideHost_Log( "grDrawVertexArray: mode %u ignored", mode );
        return;
    }
    flush_front( );
}

// A Glide 3 GrTexInfo as the Glide 2 one OpenGLide understands. The
// structure is the same; two of its fields count the other way.
GrTexInfo to_g2( const GrTexInfo *info )
{
    GrTexInfo t = *info;
    t.smallLod = G3_LOD_TO_G2( info->smallLod );
    t.largeLod = G3_LOD_TO_G2( info->largeLod );
    t.aspectRatio = G3_ASPECT_TO_G2( info->aspectRatio );
    return t;
}

// grGet's answers are FxI32 arrays of an exact length
FxU32 answer( FxI32 *params, FxU32 plength, const FxI32 *v, FxU32 n )
{
    if ( !params || plength < n * sizeof( FxI32 ) )
    {
        return 0;
    }
    memcpy( params, v, n * sizeof( FxI32 ) );
    return n * sizeof( FxI32 );
}

} // namespace

extern "C" {

//*************************************************
//* What Glide 3 re-encoded under a Glide 2 name
//*************************************************

FX_ENTRY void FX_CALL
wrap3x_grGlideInit( void )
{
    grGlideInit( );
    reset_state( );
    apply_hints( );
    GlideHost_Log( "Glide 3 layer: grGlideInit" );
}

FX_ENTRY GrContext_t FX_CALL
wrap3x_grSstWinOpen( FxU hwnd, GrScreenResolution_t res, GrScreenRefresh_t ref,
                     GrColorFormat_t cformat, GrOriginLocation_t org,
                     int nColBuffers, int nAuxBuffers )
{
    if ( !grSstWinOpen( hwnd, res, ref, cformat, org, nColBuffers, nAuxBuffers ) )
    {
        return 0;
    }
    // a new window's viewport is all of it (3dfx's grSstWinOpen does the
    // same), and OpenGLide's open reset the per-TMU w hint
    g3.vp[ 0 ] = g3.vp[ 1 ] = 0;
    g3.vp[ 2 ] = (FxI16)Glide.WindowWidth;
    g3.vp[ 3 ] = (FxI16)Glide.WindowHeight;
    g3.coords = GR_WINDOW_COORDS;
    apply_hints( );
    GlideHost_Log( "Glide 3 layer: grSstWinOpen %ux%u", Glide.WindowWidth,
                   Glide.WindowHeight );
    // any non-zero value is a context, and there is only ever one
    return 1;
}

FX_ENTRY FxBool FX_CALL
wrap3x_grSstWinClose( GrContext_t context )
{
    grSstWinClose( );
    return FXTRUE;
}

FX_ENTRY void FX_CALL
wrap3x_grDrawPoint( const void *pt )
{
    GrVertex a;
    to_vertex( pt, &a );
    grDrawPoint( &a );
}

FX_ENTRY void FX_CALL
wrap3x_grDrawLine( const void *v1, const void *v2 )
{
    GrVertex a, b;
    to_vertex( v1, &a );
    to_vertex( v2, &b );
    grDrawLine( &a, &b );
}

FX_ENTRY void FX_CALL
wrap3x_grDrawTriangle( const void *va, const void *vb, const void *vc )
{
    GrVertex a, b, c;
    to_vertex( va, &a );
    to_vertex( vb, &b );
    to_vertex( vc, &c );
    grDrawTriangle( &a, &b, &c );
}

FX_ENTRY void FX_CALL
wrap3x_grAADrawTriangle( const void *va, const void *vb, const void *vc,
                         FxBool ab, FxBool bc, FxBool ca )
{
    GrVertex a, b, c;
    to_vertex( va, &a );
    to_vertex( vb, &b );
    to_vertex( vc, &c );
    grAADrawTriangle( &a, &b, &c, ab, bc, ca );
}

FX_ENTRY void FX_CALL
wrap3x_grTexSource( GrChipID_t tmu, FxU32 startAddress, FxU32 evenOdd,
                    GrTexInfo *info )
{
    if ( tmu <= GR_TMU1 )
    {
        // clip-space s and t are in [0,1]; the hardware wants texels, and
        // the texture's longer side is always 256 of them
        int a = info->aspectRatio;
        sscale[ tmu ] = a >= 0 ? 256.0f : (float)( 256 >> -a );
        tscale[ tmu ] = a >= 0 ? (float)( 256 >> a ) : 256.0f;
    }
    GrTexInfo t = to_g2( info );
    grTexSource( tmu, startAddress, evenOdd, &t );
}

FX_ENTRY void FX_CALL
wrap3x_grTexDownloadMipMap( GrChipID_t tmu, FxU32 startAddress, FxU32 evenOdd,
                            GrTexInfo *info )
{
    GrTexInfo t = to_g2( info );
    grTexDownloadMipMap( tmu, startAddress, evenOdd, &t );
}

FX_ENTRY FxU32 FX_CALL
wrap3x_grTexTextureMemRequired( FxU32 evenOdd, GrTexInfo *info )
{
    GrTexInfo t = to_g2( info );
    return grTexTextureMemRequired( evenOdd, &t );
}

FX_ENTRY FxU32 FX_CALL
wrap3x_grTexCalcMemRequired( GrLOD_t lodmin, GrLOD_t lodmax,
                             GrAspectRatio_t aspect, GrTextureFormat_t fmt )
{
    return grTexCalcMemRequired( G3_LOD_TO_G2( lodmin ), G3_LOD_TO_G2( lodmax ),
                                 G3_ASPECT_TO_G2( aspect ), fmt );
}

FX_ENTRY void FX_CALL
wrap3x_grTexDownloadMipMapLevel( GrChipID_t tmu, FxU32 startAddress,
                                 GrLOD_t thisLod, GrLOD_t largeLod,
                                 GrAspectRatio_t aspect, GrTextureFormat_t fmt,
                                 FxU32 evenOdd, void *data )
{
    grTexDownloadMipMapLevel( tmu, startAddress, G3_LOD_TO_G2( thisLod ),
                              G3_LOD_TO_G2( largeLod ), G3_ASPECT_TO_G2( aspect ),
                              fmt, evenOdd, data );
}

FX_ENTRY FxBool FX_CALL
wrap3x_grTexDownloadMipMapLevelPartial( GrChipID_t tmu, FxU32 startAddress,
                                        GrLOD_t thisLod, GrLOD_t largeLod,
                                        GrAspectRatio_t aspect,
                                        GrTextureFormat_t fmt, FxU32 evenOdd,
                                        void *data, int start, int end )
{
    grTexDownloadMipMapLevelPartial( tmu, startAddress, G3_LOD_TO_G2( thisLod ),
                                     G3_LOD_TO_G2( largeLod ),
                                     G3_ASPECT_TO_G2( aspect ), fmt, evenOdd,
                                     data, start, end );
    return FXTRUE;
}

// the texture tables lost their TMU argument; OpenGLide has one TMU
FX_ENTRY void FX_CALL
wrap3x_grTexDownloadTable( GrTexTable_t type, void *data )
{
    grTexDownloadTable( GR_TMU0, type, data );
}

FX_ENTRY void FX_CALL
wrap3x_grTexDownloadTablePartial( GrTexTable_t type, void *data, int start,
                                  int end )
{
    grTexDownloadTablePartial( GR_TMU0, type, data, start, end );
}

FX_ENTRY void FX_CALL
wrap3x_grTexNCCTable( GrNCCTable_t table )
{
    grTexNCCTable( GR_TMU0, table );
}

FX_ENTRY FxBool FX_CALL
wrap3x_grLfbWriteRegion( GrBuffer_t dst_buffer, FxU32 dst_x, FxU32 dst_y,
                         GrLfbSrcFmt_t src_format, FxU32 src_width,
                         FxU32 src_height, FxBool pixelPipeline,
                         FxI32 src_stride, void *src_data )
{
    // OpenGLide's LFB writes never go through the pixel pipeline, and
    // grGet(GR_LFB_PIXEL_PIPE) says so
    return grLfbWriteRegion( dst_buffer, dst_x, dst_y, src_format, src_width,
                             src_height, src_stride, src_data );
}

FX_ENTRY void FX_CALL
wrap3x_grGlideGetState( void *state )
{
    grGlideGetState( (GrState *)state );
    memcpy( (FxU8 *)state + sizeof( GlideState ), &g3, sizeof( g3 ) );
}

FX_ENTRY void FX_CALL
wrap3x_grGlideSetState( const void *state )
{
    grGlideSetState( (const GrState *)state );
    memcpy( &g3, (const FxU8 *)state + sizeof( GlideState ), sizeof( g3 ) );
    apply_hints( );
}

// a .3df header's level of detail and aspect ratio, answered in Glide 3's
// encoding
static void header_to_g3( Gu3dfInfo *info )
{
    info->header.small_lod = G2_LOD_TO_G3( info->header.small_lod );
    info->header.large_lod = G2_LOD_TO_G3( info->header.large_lod );
    info->header.aspect_ratio = G2_ASPECT_TO_G3( info->header.aspect_ratio );
}

FX_ENTRY FxBool FX_CALL
wrap3x_gu3dfGetInfo( const char *filename, Gu3dfInfo *info )
{
    FxBool ok = gu3dfGetInfo( filename, info );
    if ( ok )
    {
        header_to_g3( info );
    }
    return ok;
}

FX_ENTRY FxBool FX_CALL
wrap3x_gu3dfLoad( const char *filename, Gu3dfInfo *info )
{
    FxBool ok = gu3dfLoad( filename, info );
    if ( ok )
    {
        header_to_g3( info );
    }
    return ok;
}

//*************************************************
//* What Glide 3 added
//*************************************************

FX_ENTRY void FX_CALL
grVertexLayout( FxU32 param, FxI32 offset, FxU32 mode )
{
    FxI16 off = ( mode == GR_PARAM_ENABLE ) ? (FxI16)offset : -1;
    Layout &L = g3.layout;

    switch ( param )
    {
    case GR_PARAM_XY:       L.xy = off; break;
    case GR_PARAM_Z:        L.z = off; break;
    case GR_PARAM_W:        L.w = off; break;
    case GR_PARAM_Q:        L.q = off; break;
    case GR_PARAM_FOG_EXT:  L.fog = off; break;
    case GR_PARAM_ST0:      L.st0 = off; break;
    case GR_PARAM_ST1:      L.st1 = off; break;
    case GR_PARAM_Q0:       L.q0 = off; break;
    case GR_PARAM_Q1:       L.q1 = off; break;
    // a packed colour and the float ones exclude each other, the way
    // qemu-3dfx's guest DLL decides how many bytes of a vertex to send
    case GR_PARAM_A:        L.a = off; if ( off >= 0 ) L.pargb = -1; break;
    case GR_PARAM_RGB:      L.rgb = off; if ( off >= 0 ) L.pargb = -1; break;
    case GR_PARAM_PARGB:
        L.pargb = off;
        if ( off >= 0 )
        {
            L.a = L.rgb = -1;
        }
        break;
    default:
        GlideHost_Log( "grVertexLayout: parameter %#x ignored", param );
        break;
    }
}

FX_ENTRY void FX_CALL
grDrawVertexArray( FxU32 mode, FxU32 count, void *pointers )
{
    void **v = (void **)pointers;
    draw_array( mode, count, [v]( FxU32 i ) { return (const void *)v[ i ]; } );
}

FX_ENTRY void FX_CALL
grDrawVertexArrayContiguous( FxU32 mode, FxU32 count, void *vertices,
                             FxU32 stride )
{
    const FxU8 *base = (const FxU8 *)vertices;
    draw_array( mode, count,
                [base, stride]( FxU32 i ) { return (const void *)( base + i * stride ); } );
}

FX_ENTRY void FX_CALL
grGlideGetVertexLayout( void *layout )
{
    memcpy( layout, &g3.layout, sizeof( Layout ) );
}

FX_ENTRY void FX_CALL
grGlideSetVertexLayout( const void *layout )
{
    memcpy( &g3.layout, layout, sizeof( Layout ) );
}

FX_ENTRY void FX_CALL
grCoordinateSpace( FxU32 mode )
{
    g3.coords = ( mode == GR_CLIP_COORDS ) ? GR_CLIP_COORDS : GR_WINDOW_COORDS;
}

FX_ENTRY void FX_CALL
grViewport( FxI32 x, FxI32 y, FxI32 width, FxI32 height )
{
    g3.vp[ 0 ] = (FxI16)x;
    g3.vp[ 1 ] = (FxI16)y;
    g3.vp[ 2 ] = (FxI16)width;
    g3.vp[ 3 ] = (FxI16)height;
}

FX_ENTRY void FX_CALL
grDepthRange( float n, float f )
{
    g3.depth[ 0 ] = n;
    g3.depth[ 1 ] = f;
}

FX_ENTRY void FX_CALL
grEnable( FxU32 mode )
{
    if ( mode < 16 )
    {
        g3.enables |= 1u << mode;
    }
    if ( mode == GR_SHAMELESS_PLUG )
    {
        grGlideShamelessPlug( FXTRUE );
    }
}

FX_ENTRY void FX_CALL
grDisable( FxU32 mode )
{
    if ( mode < 16 )
    {
        g3.enables &= ~( 1u << mode );
    }
    if ( mode == GR_SHAMELESS_PLUG )
    {
        grGlideShamelessPlug( FXFALSE );
    }
}

// What a game can ask of the "hardware". The answers describe what this
// wrapper is — one TMU, 256-texel textures, a 16-bit 565 frame buffer and
// depth buffer like the Voodoo Graphics it claims to be — rather than a
// board it is not. Answered before grGlideInit too: hw/3dfx lets grGet
// through first, and games ask how many boards there are.
FX_ENTRY FxU32 FX_CALL
grGet( FxU32 pname, FxU32 plength, FxI32 *params )
{
    FxU32 tris_in = 0, tris_out = 0;
    FxI32 texmem = Glide.TextureMemory ? Glide.TextureMemory : 4 << 20;

    switch ( pname )
    {
    case GR_BITS_DEPTH:
    {
        const FxI32 v[] = { 16 };
        return answer( params, plength, v, 1 );
    }
    case GR_BITS_RGBA:
    {
        const FxI32 v[] = { 5, 6, 5, 0 };
        return answer( params, plength, v, 4 );
    }
    case GR_FIFO_FULLNESS:
    {
        // never full: the FIFO a game would pace itself by is hw/3dfx's
        const FxI32 v[] = { ( 0xffff << 8 ) + ( 0xffff >> 8 ), 0xffff };
        return answer( params, plength, v, 2 );
    }
    case GR_FOG_TABLE_ENTRIES:
    {
        const FxI32 v[] = { GR_FOG_TABLE_SIZE };
        return answer( params, plength, v, 1 );
    }
    case GR_GAMMA_TABLE_ENTRIES:
    {
        const FxI32 v[] = { 32 };
        return answer( params, plength, v, 1 );
    }
    case GR_BITS_GAMMA:
    {
        const FxI32 v[] = { 8 };
        return answer( params, plength, v, 1 );
    }
    case GR_GLIDE_STATE_SIZE:
    {
        const FxI32 v[] = { (FxI32)( sizeof( GlideState ) + sizeof( G3State ) ) };
        return answer( params, plength, v, 1 );
    }
    case GR_GLIDE_VERTEXLAYOUT_SIZE:
    {
        const FxI32 v[] = { (FxI32)sizeof( Layout ) };
        return answer( params, plength, v, 1 );
    }
    case GR_IS_BUSY:
    case GR_LFB_PIXEL_PIPE:
    case GR_MEMORY_UMA:
    case GR_NON_POWER_OF_TWO_TEXTURES:
    case GR_PENDING_BUFFERSWAPS:
    case GR_STATS_LINES:
    case GR_STATS_PIXELS_AFUNC_FAIL:
    case GR_STATS_PIXELS_CHROMA_FAIL:
    case GR_STATS_PIXELS_DEPTHFUNC_FAIL:
    case GR_STATS_PIXELS_IN:
    case GR_STATS_PIXELS_OUT:
    case GR_STATS_POINTS:
    {
        const FxI32 v[] = { 0 };
        return answer( params, plength, v, 1 );
    }
    case GR_MAX_TEXTURE_SIZE:
    {
        const FxI32 v[] = { 256 };
        return answer( params, plength, v, 1 );
    }
    case GR_MAX_TEXTURE_ASPECT_RATIO:
    {
        const FxI32 v[] = { 3 };
        return answer( params, plength, v, 1 );
    }
    case GR_MEMORY_FB:
    {
        const FxI32 v[] = { 4 << 20 };
        return answer( params, plength, v, 1 );
    }
    case GR_MEMORY_TMU:
    {
        const FxI32 v[] = { texmem };
        return answer( params, plength, v, 1 );
    }
    case GR_NUM_BOARDS:
    case GR_NUM_FB:
    case GR_NUM_TMU:
    case GR_REVISION_FB:
    case GR_REVISION_TMU:
    case GR_SUPPORTS_PASSTHRU:
    {
        const FxI32 v[] = { 1 };
        return answer( params, plength, v, 1 );
    }
    case GR_NUM_SWAP_HISTORY_BUFFER:
    {
        const FxI32 v[] = { 8 };
        return answer( params, plength, v, 1 );
    }
    case GR_STATS_TRIANGLES_IN:
    case GR_STATS_TRIANGLES_OUT:
    {
        grTriStats( &tris_in, &tris_out );
        const FxI32 v[] = { (FxI32)( pname == GR_STATS_TRIANGLES_IN ? tris_in : tris_out ) };
        return answer( params, plength, v, 1 );
    }
    case GR_SWAP_HISTORY:
    {
        const FxI32 v[ 8 ] = { 0 };
        return answer( params, plength, v, 8 );
    }
    case GR_TEXTURE_ALIGN:
    {
        const FxI32 v[] = { 8 };
        return answer( params, plength, v, 1 );
    }
    case GR_VIDEO_POSITION:
    {
        const FxI32 v[] = { 0, 0 };
        return answer( params, plength, v, 2 );
    }
    case GR_VIEWPORT:
    {
        const FxI32 v[] = { g3.vp[ 0 ], g3.vp[ 1 ], g3.vp[ 2 ], g3.vp[ 3 ] };
        return answer( params, plength, v, 4 );
    }
    case GR_WDEPTH_MIN_MAX:
    {
        const FxI32 v[] = { 0x0000, 0xffff };
        return answer( params, plength, v, 2 );
    }
    case GR_ZDEPTH_MIN_MAX:
    {
        const FxI32 v[] = { 0xffff, 0x0000 };
        return answer( params, plength, v, 2 );
    }
    default:
        GlideHost_Log( "grGet: pname %#x not answered", pname );
        return 0;
    }
}

// hw/3dfx copies GR_EXTENSION, GR_HARDWARE and GR_VERSION into the guest
// DLL at grGlideInit (192, 16 and 32 bytes), which answers grGetString
// from those copies from then on. No extension is claimed yet: a game
// that finds one by name calls through grGetProcAddress, which has none.
FX_ENTRY const char * FX_CALL
grGetString( FxU32 pname )
{
    switch ( pname )
    {
    case GR_EXTENSION:  return "";
    case GR_HARDWARE:   return "Voodoo Graphics";
    case GR_RENDERER:   return "Glide";
    case GR_VENDOR:     return "3Dfx Interactive";
    case GR_VERSION:    return "3.01.00";
    default:            return "ERROR";
    }
}

FX_ENTRY void * FX_CALL
grGetProcAddress( char *procName )
{
    GlideHost_Log( "grGetProcAddress(%s): none", procName ? procName : "(null)" );
    return NULL;
}

FX_ENTRY FxBool FX_CALL
grReset( FxU32 what )
{
    switch ( what )
    {
    case GR_VERTEX_PARAMETER:
        reset_layout( );
        return FXTRUE;
    case GR_STATS_LINES:
    case GR_STATS_PIXELS:
    case GR_STATS_POINTS:
    case GR_STATS_TRIANGLES:
        grResetTriStats( );
        return FXTRUE;
    default:
        return FXFALSE;
    }
}

// The resolutions glidewnd.c can open (its table is indexed by
// GrScreenResolution_t, 0 to 15), at 60 Hz, double or triple buffered,
// with or without a depth buffer. The size in bytes when output is NULL.
FX_ENTRY FxI32 FX_CALL
grQueryResolutions( const GrResolution *tmpl, GrResolution *output )
{
    FxI32 size = 0;

    for ( int res = 0; res <= 0xf; res++ )
    {
        for ( int col = 2; col <= 3; col++ )
        {
            for ( int aux = 0; aux <= 1; aux++ )
            {
                if ( tmpl
                     && ( ( (FxU32)tmpl->resolution != GR_QUERY_ANY && tmpl->resolution != res )
                          || ( (FxU32)tmpl->refresh != GR_QUERY_ANY && tmpl->refresh != GR_REFRESH_60Hz )
                          || ( (FxU32)tmpl->numColorBuffers != GR_QUERY_ANY && tmpl->numColorBuffers != col )
                          || ( (FxU32)tmpl->numAuxBuffers != GR_QUERY_ANY && tmpl->numAuxBuffers != aux ) ) )
                {
                    continue;
                }
                if ( output )
                {
                    output->resolution = res;
                    output->refresh = GR_REFRESH_60Hz;
                    output->numColorBuffers = col;
                    output->numAuxBuffers = aux;
                    output++;
                }
                size += sizeof( GrResolution );
            }
        }
    }
    return size;
}

FX_ENTRY FxBool FX_CALL
grSelectContext( GrContext_t context )
{
    return ( context && OpenGL.WinOpen ) ? FXTRUE : FXFALSE;
}

FX_ENTRY void FX_CALL
grFinish( void )
{
    RenderDrawTriangles( );
    glFinish( );
}

FX_ENTRY void FX_CALL
grFlush( void )
{
    RenderDrawTriangles( );
    glFlush( );
}

// OpenGLide has one gamma value, not a ramp; a table is not approximated
// yet, and says so once
FX_ENTRY void FX_CALL
grLoadGammaTable( FxU32 nentries, FxU32 *red, FxU32 *green, FxU32 *blue )
{
    static bool said;
    if ( !said )
    {
        said = true;
        GlideHost_Log( "grLoadGammaTable(%u entries): not applied", nentries );
    }
}

FX_ENTRY void FX_CALL
guGammaCorrectionRGB( float red, float green, float blue )
{
    grGammaCorrectionValue( ( red + green + blue ) / 3.0f );
}

// Two Voodoo tuning calls both APIs export and OpenGLide lacked, which
// left hw/3dfx blocking them as null pointers: nothing to tune here.
FX_ENTRY void FX_CALL
grSstConfigPipeline( GrChipID_t chip, FxU32 reg, FxU32 value )
{
}

FX_ENTRY void FX_CALL
grSstVidMode( FxU32 whichSst, void *vidTimings )
{
}

} // extern "C"
