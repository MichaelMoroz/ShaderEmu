/*
 * q3_snap: Quake III's server telling its clients what they see, every server frame. Copied from ioq3 with the
 * machine's patch (linux/quake3/quake3.patch): MSG_WriteBits (the patch's path: a message's bits put in at once,
 * no Huffman), MSG_WriteDeltaEntity with the patch's whole-state comparison first and the real entityStateFields
 * table, SV_EmitPacketEntities' walk of the old and new lists; SV_AddEntitiesVisibleFromPoint's tests stand as a
 * cluster bit of the client's PVS for each entity. 256 entities, half of them moving (positions, times, angles,
 * now and then an event), 3 clients each with its own PVS; a unit is one server frame's three messages.
 */
#include <string.h>
#include "wl.h"

#define GENTITYNUM_BITS 10
#define MAX_GENTITIES (1 << GENTITYNUM_BITS)
#define FLOAT_INT_BITS 13
#define FLOAT_INT_BIAS (1 << (FLOAT_INT_BITS - 1))
#define ENTITIES 256
#define CLIENTS 3
#define CLUSTERS 128
#define FIELDS 51
#define MSG_BYTES 16384

typedef struct { int number; int word[FIELDS]; } entityState_t;   /* (52 words, as the real one) */
typedef struct { unsigned char *data; int maxsize, cursize, bit, overflowed; } msg_t;
typedef struct { int offset, bits; } netField_t;

/* entityStateFields' bits in its order (pos.trTime 32, pos.trBase[0] float, ...); the offsets a permutation of the words */
static const unsigned char field_bits[FIELDS] = {
    32, 0, 0, 0, 0, 0, 0, 0, 0, 10, 0, 8, 8, 8, 8, GENTITYNUM_BITS, 8, 19, GENTITYNUM_BITS, 8, 8, 0, 32, 8, 0, 0, 0,
    24, 16, 8, GENTITYNUM_BITS, 8, 8, 0, 0, 0, 8, 0, 32, 32, 32, 0, 0, 0, 0, 32, 0, 0, 0, 32, 16};
static netField_t entityStateFields[FIELDS];
static entityState_t baseline[ENTITIES], frame_old[ENTITIES], frame_new[ENTITIES];
static unsigned char cluster_of[ENTITIES], pvs[2][CLIENTS][CLUSTERS / 8];

static void MSG_WriteBits(msg_t *msg, int value, int bits)
{
    if (msg->overflowed) return;
    if (bits < 0) bits = -bits;
    value &= (int)(0xffffffffu >> (32 - bits));
    if (msg->bit + bits < msg->maxsize << 3) {
        int at = msg->bit, sh = at & 7, n = (sh + bits + 7) >> 3, k;
        unsigned char *p = msg->data + (at >> 3);
        p[0] = (unsigned char)((sh ? p[0] : 0) | (unsigned char)((unsigned)value << sh));
        for (k = 1; k < n; k++) p[k] = (unsigned char)((unsigned)value >> (8 * k - sh));
        msg->bit = at + bits;
        msg->cursize = (msg->bit >> 3) + 1;
        return;
    }
    msg->overflowed = 1;
}

static void MSG_WriteDeltaEntity(msg_t *msg, const entityState_t *from, const entityState_t *to, int force)
{
    int i, lc = 0, differ = 0;
    const int *a = (const int *)from, *b = (const int *)to;
    if (!to) {
        if (!from) return;
        MSG_WriteBits(msg, from->number, GENTITYNUM_BITS);
        MSG_WriteBits(msg, 1, 1);
        return;
    }
    for (i = 0; i < (int)(sizeof(*from) / 4); i++) differ |= a[i] ^ b[i];
    if (differ)
        for (i = 0; i < FIELDS; i++)
            if (a[entityStateFields[i].offset] != b[entityStateFields[i].offset]) lc = i + 1;
    if (lc == 0) {
        if (!force) return;
        MSG_WriteBits(msg, to->number, GENTITYNUM_BITS);
        MSG_WriteBits(msg, 0, 1);
        MSG_WriteBits(msg, 0, 1);
        return;
    }
    MSG_WriteBits(msg, to->number, GENTITYNUM_BITS);
    MSG_WriteBits(msg, 0, 1);
    MSG_WriteBits(msg, 1, 1);
    MSG_WriteBits(msg, lc, 8);
    for (i = 0; i < lc; i++) {
        const netField_t *field = &entityStateFields[i];
        const int *fromF = a + field->offset, *toF = b + field->offset;
        if (*fromF == *toF) { MSG_WriteBits(msg, 0, 1); continue; }
        MSG_WriteBits(msg, 1, 1);
        if (field->bits == 0) {
            float fullFloat;
            memcpy(&fullFloat, toF, 4);
            int trunc = (int)fullFloat;
            if (fullFloat == 0.0f) MSG_WriteBits(msg, 0, 1);
            else {
                MSG_WriteBits(msg, 1, 1);
                if (trunc == fullFloat && trunc + FLOAT_INT_BIAS >= 0 && trunc + FLOAT_INT_BIAS < (1 << FLOAT_INT_BITS)) {
                    MSG_WriteBits(msg, 0, 1);
                    MSG_WriteBits(msg, trunc + FLOAT_INT_BIAS, FLOAT_INT_BITS);
                } else {
                    MSG_WriteBits(msg, 1, 1);
                    MSG_WriteBits(msg, *toF, 32);
                }
            }
        } else if (*toF == 0) MSG_WriteBits(msg, 0, 1);
        else {
            MSG_WriteBits(msg, 1, 1);
            MSG_WriteBits(msg, *toF, field->bits);
        }
    }
}

/* SV_EmitPacketEntities: the two frames' visible lists, in entity order */
static void SV_EmitPacketEntities(const unsigned char *from_list, int from_count, const unsigned char *to_list, int to_count, msg_t *msg)
{
    int oldindex = 0, newindex = 0;
    while (newindex < to_count || oldindex < from_count) {
        int newnum = newindex >= to_count ? 9999 : to_list[newindex];
        int oldnum = oldindex >= from_count ? 9999 : from_list[oldindex];
        if (newnum == oldnum) {
            MSG_WriteDeltaEntity(msg, &frame_old[oldnum], &frame_new[newnum], 0);
            oldindex++, newindex++;
        } else if (newnum < oldnum) {
            MSG_WriteDeltaEntity(msg, &baseline[newnum], &frame_new[newnum], 1);
            newindex++;
        } else {
            MSG_WriteDeltaEntity(msg, &frame_old[oldnum], 0, 1);
            oldindex++;
        }
    }
    MSG_WriteBits(msg, MAX_GENTITIES - 1, GENTITYNUM_BITS);
}

struct scratch { unsigned char data[MSG_BYTES]; unsigned char from_list[ENTITIES], to_list[ENTITIES]; };

static void setup(void)
{
    uint32_t seed = 0x51a9;
    for (int i = 0; i < FIELDS; i++) entityStateFields[i].offset = 1 + (i * 17) % FIELDS, entityStateFields[i].bits = field_bits[i];
    for (int e = 0; e < ENTITIES; e++) {
        entityState_t *s = &baseline[e];
        s->number = e;
        for (int i = 0; i < FIELDS; i++) {
            const netField_t *f = &entityStateFields[i];
            if (wl_rand(&seed) % 3) { s->word[f->offset - 1] = 0; continue; }
            if (f->bits == 0) {
                float v = (wl_rand(&seed) & 1) ? (float)((int)(wl_rand(&seed) % 4000) - 2000) : 4000 * wl_randf(&seed) - 2000;
                memcpy(&s->word[f->offset - 1], &v, 4);
            } else s->word[f->offset - 1] = (int)(wl_rand(&seed) & (f->bits == 32 ? 0xffffffu : (1u << f->bits) - 1));
        }
        frame_old[e] = *s;
        frame_new[e] = *s;
        cluster_of[e] = (unsigned char)(wl_rand(&seed) % CLUSTERS);
        if (e % 2) {   /* a moving thing: position, angles, times change; now and then an event */
            for (int i = 0; i < 9; i++) {
                float v;
                memcpy(&v, &frame_old[e].word[entityStateFields[1 + i].offset - 1], 4);
                v += 3.5f + 10 * wl_randf(&seed);
                memcpy(&frame_new[e].word[entityStateFields[1 + i].offset - 1], &v, 4);
            }
            frame_new[e].word[entityStateFields[0].offset - 1] += 50;
            if (e % 7 == 1) frame_new[e].word[entityStateFields[9].offset - 1] ^= 0x105;
        }
    }
    for (int f = 0; f < 2; f++)
        for (int c = 0; c < CLIENTS; c++)
            for (int k = 0; k < CLUSTERS / 8; k++) pvs[f][c][k] = (unsigned char)(wl_rand(&seed) | wl_rand(&seed));
}

static void *inst(void) { return wl_alloc(sizeof(struct scratch)); }

static uint32_t run(void *ctx)
{
    struct scratch *s = ctx;
    uint32_t sum = 0;
    for (int c = 0; c < CLIENTS; c++) {
        int from_count = 0, to_count = 0;
        for (int e = 0; e < ENTITIES; e++) {   /* (SV_AddEntitiesVisibleFromPoint: the cluster's bit in the PVS) */
            int k = cluster_of[e];
            if (pvs[0][c][k >> 3] & (1 << (k & 7))) s->from_list[from_count++] = (unsigned char)e;
            if (pvs[1][c][k >> 3] & (1 << (k & 7))) s->to_list[to_count++] = (unsigned char)e;
        }
        msg_t msg = {s->data, MSG_BYTES, 0, 0, 0};
        SV_EmitPacketEntities(s->from_list, from_count, s->to_list, to_count, &msg);
        for (int i = 0; i < msg.cursize; i += 4) sum = sum * 33 + msg.data[i];
        sum += (uint32_t)msg.bit;
    }
    return sum;
}

const wl_kernel wl_q3_snap = {"q3_snap", "Quake III server: snapshots, SV_EmitPacketEntities and MSG_WriteDeltaEntity", setup, inst, run};
