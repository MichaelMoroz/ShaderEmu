/*
 * ClassiCube on the world's LAN (docs/games/classicube.md, "Played together"): a player's own singleplayer world served to
 * others in ClassiCube's own protocol (Classic, version 7), so that a joiner is the stock client connecting to
 * 10.0.0.N:25565. The host is this game's own loop: twenty times a second (the singleplayer server's tick) it takes new
 * players, reads what they sent and writes what is owed, all without waiting:
 *   - a joiner gets the level gzipped by libmedia's deflate on the worker cores (media_gzip), in 1,024-byte chunks;
 *   - a block placed or taken away by anybody goes to everybody as one SetBlock of 8 bytes (the host applies a joiner's);
 *   - every player's place goes to the others when it moved, at most 10 times a second, 10 bytes each;
 *   - the host shows joiners with its protocol's own handlers (models, names, smoothed moves), as a client would.
 * A beacon a few times a second (UDP broadcast, port 25566) says a game is hosted, which "Join a LAN game" listens for and
 * then runs the stock client against it. CLASSICUBE_LAN=host hosts from the start; CLASSICUBE_LAN_LOG=1 says each join,
 * block and second's bytes.
 */
#include "Core.h"
#include "Game.h"
#include "World.h"
#include "Entity.h"
#include "Chat.h"
#include "String_.h"
#include "Server.h"
#include "Protocol.h"
#include "ExtMath.h"
#include "Utils.h"
#include "BlockID.h"
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <net/if.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <unistd.h>
#include "media.h"

#define LAN_PORT 25565
#define LAN_BEACON_PORT 25566
#define LAN_MOST 8                 /* joiners at once */
#define LAN_POS_MS 100             /* a player's place told at most this often */

uint32_t SE_Milliseconds(void);
void Protocol_SE_Install(void);    /* classicube.patch: the protocol's handlers in a singleplayer game */

struct lan_player {
	int fd, id;                    /* the socket (-1: free); the player's number for the others (1 to LAN_MOST) */
	int named;                     /* its identification came: it has the level, or is getting it */
	char name[65];
	uint8_t in[2048];
	uint32_t in_n;
	uint8_t *out;                  /* what is owed it, from out_at to out_n */
	uint32_t out_at, out_n, out_room;
	uint8_t pos[10];               /* its last place, as it sent it (opcode 8, its id 255) */
	uint32_t pos_told_ms, joined_ms;
	int pos_new;
};

static struct lan_player lan_players[LAN_MOST];
static int lan_listen = -1, lan_beacon = -1, lan_hosting, lan_log = -1;
static uint32_t lan_beacon_ms, lan_stat_ms, lan_bytes_out, lan_bytes_in, lan_host_pos_ms;
static uint8_t lan_host_pos[10];
static uint32_t lan_address;       /* this machine's, network order */
/* The level as last gzipped, kept while no block changed: a second joiner gets it at once */
static uint8_t *lan_gz;
static uint32_t lan_gz_n, lan_gz_gen = ~0u, lan_gen;

static void lan_say(const char *text) {
	cc_string s = String_FromReadonly(text);
	Chat_Add(&s);
	if (lan_log) { printf("lan: %s\n", text); fflush(stdout); }
}

static void lan_send(struct lan_player *p, const void *data, uint32_t n) {
	if (p->fd < 0) return;
	if (p->out_n + n > p->out_room) {
		/* (what was sent is dropped from the front first) */
		if (p->out_at) {
			memmove(p->out, p->out + p->out_at, p->out_n - p->out_at);
			p->out_n -= p->out_at, p->out_at = 0;
		}
		if (p->out_n + n > p->out_room) {
			uint32_t room = (p->out_n + n) * 2 + 4096;
			uint8_t *more = realloc(p->out, room);
			if (!more) return;
			p->out = more, p->out_room = room;
		}
	}
	memcpy(p->out + p->out_n, data, n);
	p->out_n += n;
}

static void lan_write(struct lan_player *p);

static void lan_send_all(const void *data, uint32_t n, int but) {
	int i;
	for (i = 0; i < LAN_MOST; i++)
		if (lan_players[i].fd >= 0 && lan_players[i].named == 2 && lan_players[i].id != but) lan_send(&lan_players[i], data, n);
}

static void put_name(uint8_t *to, const char *name) {
	size_t n = strlen(name);
	memset(to, ' ', 64);
	memcpy(to, name, n > 64 ? 64 : n);
}

/* The host's own place in the protocol's units: 1/32 of a block, y at the eye's height (feet + 51/32) */
static void host_place(uint8_t *pkt, int id) {
	struct Entity *e = &Entities.CurPlayer->Base;
	pkt[0] = OPCODE_ENTITY_TELEPORT;
	pkt[1] = (uint8_t)id;
	Mem_WriteU16_BE(pkt + 2, (int)(e->Position.x * 32));
	Mem_WriteU16_BE(pkt + 4, (int)(e->Position.y * 32) + 51);
	Mem_WriteU16_BE(pkt + 6, (int)(e->Position.z * 32));
	pkt[8] = Math_Deg2Packed(e->Yaw);
	pkt[9] = Math_Deg2Packed(e->Pitch);
}

static void spawn_packet(uint8_t *pkt, int id, const char *name, const uint8_t *place) {
	pkt[0] = OPCODE_ADD_ENTITY;
	pkt[1] = (uint8_t)id;
	put_name(pkt + 2, name);
	memcpy(pkt + 66, place + 2, 8);   /* x, y, z, yaw, pitch */
}

/* The host shows what a client would be told: its protocol's own handlers (models, names, smoothing) */
static void host_handle(const uint8_t *pkt) {
	if (Protocol.Handlers[pkt[0]]) Protocol.Handlers[pkt[0]]((cc_uint8 *)pkt + 1);
}

static void lan_drop(struct lan_player *p, const char *why) {
	char text[128];
	uint8_t gone[2] = { OPCODE_REMOVE_ENTITY, (uint8_t)p->id };
	if (p->fd < 0) return;
	close(p->fd);
	p->fd = -1;
	if (p->named == 2) {
		lan_send_all(gone, 2, p->id);
		host_handle(gone);
		snprintf(text, sizeof text, "%s left the game (%s)", p->name, why);
		lan_say(text);
	}
	free(p->out);
	p->out = NULL, p->out_n = p->out_at = p->out_room = 0;
	p->named = 0;
}

/* A joiner's identification: the server's, the level gzipped (on the workers), its own place and the others */
static void lan_welcome(struct lan_player *p) {
	uint8_t pkt[1028], place[10];
	uint8_t *raw, *gz;
	uint32_t gz_n = 0, at, began = SE_Milliseconds();
	int i, volume = World.Volume;
	char text[160];

	pkt[0] = OPCODE_HANDSHAKE;
	pkt[1] = 7;
	put_name(pkt + 2, "ClassiCube on the LAN");
	put_name(pkt + 66, "a ShaderEmu machine's world");
	pkt[130] = 0;
	lan_send(p, pkt, 131);
	pkt[0] = OPCODE_LEVEL_BEGIN;
	lan_send(p, pkt, 1);
	lan_write(p);	/* (the answer goes at once: the level is packed meanwhile) */
	/* the level: its volume (4 bytes, big end first) and then its blocks, x fastest, then z, then y: gzipped, again only
	   after a block changed */
	if (lan_gz_gen != lan_gen || !lan_gz) {
		raw = malloc((size_t)volume + 4);
		if (!raw) { lan_drop(p, "no memory for the level"); return; }
		raw[0] = (uint8_t)(volume >> 24), raw[1] = (uint8_t)(volume >> 16), raw[2] = (uint8_t)(volume >> 8), raw[3] = (uint8_t)volume;
		memcpy(raw + 4, World.Blocks, (size_t)volume);
		if (lan_gz) media_free(lan_gz, lan_gz_n);
		lan_gz = media_gzip(raw, (uint32_t)volume + 4, 4, &lan_gz_n, 255);
		free(raw);
		lan_gz_gen = lan_gen;
	}
	gz = lan_gz, gz_n = lan_gz_n;
	if (!gz) { lan_drop(p, "the level could not be packed"); return; }
	for (at = 0; at < gz_n; at += 1024) {
		uint32_t n = gz_n - at < 1024 ? gz_n - at : 1024;
		memset(pkt, 0, sizeof pkt);
		pkt[0] = OPCODE_LEVEL_DATA;
		Mem_WriteU16_BE(pkt + 1, n);
		memcpy(pkt + 3, gz + at, n);
		pkt[1027] = (uint8_t)((at + n) * 100 / gz_n);
		lan_send(p, pkt, 1028);
	}
	pkt[0] = OPCODE_LEVEL_END;
	Mem_WriteU16_BE(pkt + 1, World.Width);
	Mem_WriteU16_BE(pkt + 3, World.Height);
	Mem_WriteU16_BE(pkt + 5, World.Length);
	lan_send(p, pkt, 7);
	/* where it stands: where the host does; then the host (0) and the others it is to see */
	host_place(place, ENTITIES_SELF_ID);
	spawn_packet(pkt, ENTITIES_SELF_ID, p->name, place);
	lan_send(p, pkt, 74);
	host_place(place, 0);
	spawn_packet(pkt, 0, Game_Username.buffer ? "Host" : "Host", place);
	{
		char host_name[65];
		int n = Game_Username.length < 64 ? Game_Username.length : 64;
		memcpy(host_name, Game_Username.buffer, n), host_name[n] = 0;
		spawn_packet(pkt, 0, host_name, place);
	}
	lan_send(p, pkt, 74);
	for (i = 0; i < LAN_MOST; i++) {
		struct lan_player *o = &lan_players[i];
		if (o == p || o->fd < 0 || o->named != 2) continue;
		spawn_packet(pkt, o->id, o->name, o->pos);
		lan_send(p, pkt, 74);
	}
	/* and the others, the host too, see it come */
	p->pos[0] = OPCODE_ENTITY_TELEPORT, p->pos[1] = (uint8_t)p->id;
	memcpy(p->pos + 2, place + 2, 8);
	spawn_packet(pkt, p->id, p->name, p->pos);
	p->named = 2;
	lan_send_all(pkt, 74, p->id);
	host_handle(pkt);
	p->joined_ms = SE_Milliseconds();
	snprintf(text, sizeof text, "%s joined: the level %d bytes, gzipped on the workers to %u in %u ms", p->name, volume + 4, gz_n,
		 p->joined_ms - began);
	lan_say(text);
}

/* What a joiner sent: whole packets only */
static void lan_read(struct lan_player *p) {
	static const uint8_t sizes[256] = { [0x00] = 131, [0x05] = 9, [0x08] = 10, [0x0d] = 66 };
	ssize_t got;
	uint32_t at = 0;

	got = recv(p->fd, p->in + p->in_n, sizeof p->in - p->in_n, 0);
	if (got == 0) { lan_drop(p, "closed"); return; }
	if (got < 0) { if (errno != EAGAIN && errno != EWOULDBLOCK) lan_drop(p, "lost"); return; }
	p->in_n += (uint32_t)got;
	lan_bytes_in += (uint32_t)got;
	while (at < p->in_n) {
		uint8_t op = p->in[at], *pkt = p->in + at;
		uint32_t size = sizes[op];
		if (!size) { lan_drop(p, "a packet not of the protocol"); return; }
		if (at + size > p->in_n) break;
		if (op == 0x00 && !p->named) {
			int n = 64;
			while (n > 0 && pkt[2 + n - 1] == ' ') n--;
			memcpy(p->name, pkt + 2, n), p->name[n] = 0;
			p->named = 1;
			if (lan_log) { printf("lan: %s identified, %u ms\n", p->name, SE_Milliseconds()); fflush(stdout); }
			lan_welcome(p);
			if (p->fd < 0) return;
		} else if (op == 0x05 && p->named == 2) {
			/* a block: the host's world, then everybody's (the sender's too: what it asked is what is) */
			int x = Mem_ReadU16_BE(pkt + 1), y = Mem_ReadU16_BE(pkt + 3), z = Mem_ReadU16_BE(pkt + 5);
			BlockID block = pkt[7] ? pkt[8] : BLOCK_AIR;
			uint8_t set[8] = { OPCODE_SET_BLOCK };
			if (World_Contains(x, y, z)) {
				Game_UpdateBlock(x, y, z, block);
				lan_gen++;
				memcpy(set + 1, pkt + 1, 6);
				set[7] = (uint8_t)block;
				lan_send_all(set, 8, -1);
				if (lan_log) { printf("lan: block %d from %s at %d,%d,%d, %u ms\n", block, p->name, x, y, z, SE_Milliseconds()); fflush(stdout); }
			}
		} else if (op == 0x08 && p->named == 2) {
			memcpy(p->pos + 2, pkt + 2, 8);
			p->pos_new = 1;
		} else if (op == 0x0d && p->named == 2) {
			/* chat: the host's and everybody's, with whose it is */
			uint8_t msg[66] = { OPCODE_MESSAGE, 0 };
			char line[160];
			int n = 64;
			while (n > 0 && pkt[2 + n - 1] == ' ') n--;
			snprintf(line, sizeof line, "%s: %.*s", p->name, n, pkt + 2);
			put_name(msg + 2, line);
			lan_send_all(msg, 66, -1);
			lan_say(line);
		}
		at += size;
	}
	memmove(p->in, p->in + at, p->in_n - at);
	p->in_n -= at;
}

static void lan_write(struct lan_player *p) {
	while (p->fd >= 0 && p->out_at < p->out_n) {
		ssize_t sent = send(p->fd, p->out + p->out_at, p->out_n - p->out_at, MSG_NOSIGNAL);
		if (sent <= 0) {
			if (sent < 0 && errno != EAGAIN && errno != EWOULDBLOCK) lan_drop(p, "lost");
			return;
		}
		p->out_at += (uint32_t)sent;
		lan_bytes_out += (uint32_t)sent;
	}
	if (p->out_at == p->out_n) p->out_at = p->out_n = 0;
}

/* This machine's address, for the beacon and what the host says */
static uint32_t my_address(void) {
	struct ifreq r;
	int fd = socket(AF_INET, SOCK_DGRAM, 0);
	uint32_t a = 0;
	if (fd < 0) return 0;
	memset(&r, 0, sizeof r);
	strcpy(r.ifr_name, "lan0");
	if (ioctl(fd, SIOCGIFADDR, &r) == 0) a = ((struct sockaddr_in *)&r.ifr_addr)->sin_addr.s_addr;
	close(fd);
	return a;
}

/* Hosting on (the pause menu's "Host on the LAN", CLASSICUBE_LAN=host): false where it cannot */
int SE_LanHost(void) {
	struct sockaddr_in at;
	int one = 1, i;
	char text[96];
	if (lan_hosting) { lan_say("Already hosting on the LAN"); return 1; }
	if (!Server.IsSinglePlayer) { lan_say("Only a game of your own can be hosted"); return 0; }
	if (lan_log < 0) lan_log = getenv("CLASSICUBE_LAN_LOG") != NULL;
	lan_listen = socket(AF_INET, SOCK_STREAM, 0);
	if (lan_listen < 0) { lan_say("No network to host on"); return 0; }
	setsockopt(lan_listen, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
	memset(&at, 0, sizeof at);
	at.sin_family = AF_INET, at.sin_port = htons(LAN_PORT), at.sin_addr.s_addr = INADDR_ANY;
	if (bind(lan_listen, (struct sockaddr *)&at, sizeof at) || listen(lan_listen, 4)) {
		close(lan_listen), lan_listen = -1;
		lan_say("The LAN's port 25565 is taken");
		return 0;
	}
	fcntl(lan_listen, F_SETFL, O_NONBLOCK);
	lan_beacon = socket(AF_INET, SOCK_DGRAM, 0);
	if (lan_beacon >= 0) setsockopt(lan_beacon, SOL_SOCKET, SO_BROADCAST, &one, sizeof one);
	for (i = 0; i < LAN_MOST; i++) lan_players[i].fd = -1, lan_players[i].id = i + 1;
	Protocol_SE_Install();
	lan_hosting = 1;
	lan_address = my_address();
	lan_stat_ms = SE_Milliseconds();
	snprintf(text, sizeof text, "Hosting on the LAN at %s:%d", inet_ntoa(*(struct in_addr *)&lan_address), LAN_PORT);
	lan_say(text);
	return 1;
}

/* A block the host placed or took away (the singleplayer server's SendBlock) */
void SE_LanBlock(int x, int y, int z, BlockID now) {
	uint8_t set[8] = { OPCODE_SET_BLOCK };
	if (!lan_hosting) return;
	lan_gen++;
	Mem_WriteU16_BE(set + 1, x);
	Mem_WriteU16_BE(set + 3, y);
	Mem_WriteU16_BE(set + 5, z);
	set[7] = (uint8_t)now;
	lan_send_all(set, 8, -1);
}

/* What the host said in chat goes to everybody */
void SE_LanChat(const cc_string* text) {
	uint8_t msg[66] = { OPCODE_MESSAGE, 0 };
	char line[96];
	if (!lan_hosting || !text->length || text->buffer[0] == '/') return;
	snprintf(line, sizeof line, "%.*s: %.*s", Game_Username.length, Game_Username.buffer, text->length, text->buffer);
	put_name(msg + 2, line);
	lan_send_all(msg, 66, -1);
}

/* The singleplayer server's tick (20 a second) */
void SE_LanTick(void) {
	uint32_t now;
	int i, fd;
	static int started;

	if (!started) {
		const char *how = getenv("CLASSICUBE_LAN");
		started = 1;
		if (how && !strcmp(how, "host")) SE_LanHost();
	}
	if (!lan_hosting) return;
	now = SE_Milliseconds();
	while ((fd = accept(lan_listen, NULL, NULL)) >= 0) {
		int one = 1;
		for (i = 0; i < LAN_MOST && lan_players[i].fd >= 0; i++) {}
		if (i == LAN_MOST) { close(fd); continue; }
		fcntl(fd, F_SETFL, O_NONBLOCK);
		setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);
		memset(&lan_players[i], 0, sizeof lan_players[i]);
		lan_players[i].fd = fd, lan_players[i].id = i + 1;
		if (lan_log) { printf("lan: a connection, %u ms\n", now); fflush(stdout); }
	}
	for (i = 0; i < LAN_MOST; i++)
		if (lan_players[i].fd >= 0) lan_read(&lan_players[i]);
	/* places: each joiner's to the others and the host's screen; the host's to everybody; when moved, 10 a second */
	for (i = 0; i < LAN_MOST; i++) {
		struct lan_player *p = &lan_players[i];
		if (p->fd < 0 || p->named != 2 || !p->pos_new || now - p->pos_told_ms < LAN_POS_MS) continue;
		p->pos[0] = OPCODE_ENTITY_TELEPORT, p->pos[1] = (uint8_t)p->id;
		lan_send_all(p->pos, 10, p->id);
		host_handle(p->pos);
		p->pos_new = 0, p->pos_told_ms = now;
	}
	if (now - lan_host_pos_ms >= LAN_POS_MS) {
		uint8_t place[10];
		host_place(place, 0);
		if (memcmp(place, lan_host_pos, 10)) {
			memcpy(lan_host_pos, place, 10);
			lan_send_all(place, 10, -1);
		}
		lan_host_pos_ms = now;
	}
	for (i = 0; i < LAN_MOST; i++)
		if (lan_players[i].fd >= 0) lan_write(&lan_players[i]);
	/* the beacon, four times a second at most: "CCLAN port name" to everybody */
	if (lan_beacon >= 0 && now - lan_beacon_ms >= 250) {
		struct sockaddr_in all;
		char text[96];
		int n = snprintf(text, sizeof text, "CCLAN %d %.*s", LAN_PORT, Game_Username.length, Game_Username.buffer);
		memset(&all, 0, sizeof all);
		all.sin_family = AF_INET, all.sin_port = htons(LAN_BEACON_PORT), all.sin_addr.s_addr = inet_addr("10.0.255.255");
		sendto(lan_beacon, text, n, MSG_DONTWAIT, (struct sockaddr *)&all, sizeof all);
		lan_beacon_ms = now;
	}
	if (lan_log && now - lan_stat_ms >= 5000) {
		int players = 0;
		for (i = 0; i < LAN_MOST; i++) players += lan_players[i].fd >= 0 && lan_players[i].named == 2;
		printf("lan: %d joined, %u bytes a second out, %u in\n", players, lan_bytes_out * 1000 / (now - lan_stat_ms),
		       lan_bytes_in * 1000 / (now - lan_stat_ms));
		fflush(stdout);
		lan_bytes_out = lan_bytes_in = 0, lan_stat_ms = now;
	}
}

/* "Join a LAN game": listens a second for a host's beacon, and runs the stock client against the first heard */
void SE_LanJoin(void) {
	struct sockaddr_in at, from;
	socklen_t from_n = sizeof from;
	char text[128], name[32], ip[32], port[16];
	int fd = socket(AF_INET, SOCK_DGRAM, 0), one = 1, got = -1, heard_port = 0;
	uint32_t until = SE_Milliseconds() + 1500, me = my_address();

	if (fd < 0) { lan_say("No network to join on"); return; }
	setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
	memset(&at, 0, sizeof at);
	at.sin_family = AF_INET, at.sin_port = htons(LAN_BEACON_PORT), at.sin_addr.s_addr = INADDR_ANY;
	if (bind(fd, (struct sockaddr *)&at, sizeof at)) { close(fd); lan_say("The LAN's beacon port is taken"); return; }
	fcntl(fd, F_SETFL, O_NONBLOCK);
	while (SE_Milliseconds() < until) {
		got = recvfrom(fd, text, sizeof text - 1, 0, (struct sockaddr *)&from, &from_n);
		if (got > 0) {
			text[got] = 0;
			if (from.sin_addr.s_addr != me && sscanf(text, "CCLAN %d", &heard_port) == 1) break;
			got = -1;
		}
		usleep(20000);
	}
	close(fd);
	if (got <= 0) { lan_say("No game is hosted on the LAN (a host says so four times a second)"); return; }
	/* a name of this machine's: Player and its number on the LAN */
	snprintf(name, sizeof name, "Player%u", (unsigned)(ntohl(me) & 0xffff));
	snprintf(ip, sizeof ip, "%s", inet_ntoa(from.sin_addr));
	snprintf(port, sizeof port, "%d", heard_port);
	snprintf(text, sizeof text, "Joining %s:%s as %s", ip, port, name);
	lan_say(text);
	execl("/usr/bin/classicube", "classicube", name, "lan", ip, port, (char *)NULL);
	lan_say("The client could not be started");
}
