/*
 * NovaLK boot menu lock: persistent credential storage.  See lock_store.h.
 */

#include <string.h>
#include <printf.h>
#include <sys/types.h>
#include <platform/mt_typedefs.h>
#include <platform/mt_gpt.h>
#include "lock_store.h"

extern ssize_t partition_read(const char *part_name, off_t offset, u8 *data,
			      size_t size);
extern ssize_t partition_write(const char *part_name, off_t offset, u8 *data,
			       size_t size);
extern u64 partition_get_size_by_name(const char *part_name);
extern int partition_get_index_by_name(const char *part_name);

#define SECTOR			512
#define SLOT_AREA		(2 * SECTOR)
#define BLANK_GUARD		8192	/* must be blank in front of the slots */
#define MIN_PART_SIZE		(256 * 1024)
#define MTK_IMG_MAGIC		0x58881688

#define REC_MAGIC		0x4b4c564e	/* "NVLK" */
#define REC_VERSION		1
#define KDF_ROUNDS		4096

struct lock_rec {
	u32 magic;
	u32 version;
	u32 seq;
	u8  enabled;
	u8  type;
	u8  rsvd[2];
	u32 fail_count;
	u8  salt[16];
	u8  hash[32];
	u8  pad[440];
	u32 crc;
};

typedef char lock_rec_size_check[(sizeof(struct lock_rec) == SECTOR) ? 1 : -1];

static struct lock_rec cur;	/* state the rest of LK sees */
static int cur_slot = -1;	/* slot holding "cur", -1 = none stored yet */
static u64 part_size;
static int part_ok;
static int writable;

/* ------------------------------------------------------------------ */
/* CRC32 and SHA-256 (self contained, no dependency on other libs)    */
/* ------------------------------------------------------------------ */

static u32 crc32_calc(const u8 *p, u32 len)
{
	u32 crc = 0xffffffff;
	u32 i;
	int j;

	for (i = 0; i < len; i++) {
		crc ^= p[i];
		for (j = 0; j < 8; j++)
			crc = (crc >> 1) ^ (0xedb88320 & (0u - (crc & 1)));
	}

	return ~crc;
}

struct sha256 {
	u32 h[8];
	u8 buf[64];
	u32 blen;
	u32 total;
};

static const u32 sha_k[64] = {
	0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b,
	0x59f111f1, 0x923f82a4, 0xab1c5ed5, 0xd807aa98, 0x12835b01,
	0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7,
	0xc19bf174, 0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc,
	0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da, 0x983e5152,
	0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147,
	0x06ca6351, 0x14292967, 0x27b70a85, 0x2e1b2138, 0x4d2c6dfc,
	0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
	0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819,
	0xd6990624, 0xf40e3585, 0x106aa070, 0x19a4c116, 0x1e376c08,
	0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f,
	0x682e6ff3, 0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208,
	0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2
};

#define ROR(x, n)	(((x) >> (n)) | ((x) << (32 - (n))))

static void sha_block(struct sha256 *s, const u8 *p)
{
	u32 w[64];
	u32 a, b, c, d, e, f, g, h;
	u32 t1, t2;
	int i;

	for (i = 0; i < 16; i++)
		w[i] = ((u32)p[i * 4] << 24) | ((u32)p[i * 4 + 1] << 16) |
		       ((u32)p[i * 4 + 2] << 8) | (u32)p[i * 4 + 3];
	for (i = 16; i < 64; i++) {
		u32 s0 = ROR(w[i - 15], 7) ^ ROR(w[i - 15], 18) ^
			 (w[i - 15] >> 3);
		u32 s1 = ROR(w[i - 2], 17) ^ ROR(w[i - 2], 19) ^
			 (w[i - 2] >> 10);

		w[i] = w[i - 16] + s0 + w[i - 7] + s1;
	}

	a = s->h[0]; b = s->h[1]; c = s->h[2]; d = s->h[3];
	e = s->h[4]; f = s->h[5]; g = s->h[6]; h = s->h[7];

	for (i = 0; i < 64; i++) {
		t1 = h + (ROR(e, 6) ^ ROR(e, 11) ^ ROR(e, 25)) +
		     ((e & f) ^ (~e & g)) + sha_k[i] + w[i];
		t2 = (ROR(a, 2) ^ ROR(a, 13) ^ ROR(a, 22)) +
		     ((a & b) ^ (a & c) ^ (b & c));
		h = g; g = f; f = e; e = d + t1;
		d = c; c = b; b = a; a = t1 + t2;
	}

	s->h[0] += a; s->h[1] += b; s->h[2] += c; s->h[3] += d;
	s->h[4] += e; s->h[5] += f; s->h[6] += g; s->h[7] += h;
}

static void sha_init(struct sha256 *s)
{
	static const u32 iv[8] = {
		0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a,
		0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19
	};

	memcpy(s->h, iv, sizeof(iv));
	s->blen = 0;
	s->total = 0;
}

static void sha_update(struct sha256 *s, const u8 *p, u32 len)
{
	s->total += len;

	while (len) {
		u32 n = 64 - s->blen;

		if (n > len)
			n = len;
		memcpy(s->buf + s->blen, p, n);
		s->blen += n;
		p += n;
		len -= n;

		if (s->blen == 64) {
			sha_block(s, s->buf);
			s->blen = 0;
		}
	}
}

static void sha_final(struct sha256 *s, u8 out[32])
{
	u32 bits_hi = s->total >> 29;
	u32 bits_lo = s->total << 3;
	u8 pad = 0x80;
	u8 zero = 0;
	u8 lenb[8];
	int i;

	sha_update(s, &pad, 1);
	while (s->blen != 56)
		sha_update(s, &zero, 1);

	for (i = 0; i < 4; i++) {
		lenb[i] = (u8)(bits_hi >> (24 - 8 * i));
		lenb[4 + i] = (u8)(bits_lo >> (24 - 8 * i));
	}
	sha_update(s, lenb, 8);

	for (i = 0; i < 8; i++) {
		out[i * 4] = (u8)(s->h[i] >> 24);
		out[i * 4 + 1] = (u8)(s->h[i] >> 16);
		out[i * 4 + 2] = (u8)(s->h[i] >> 8);
		out[i * 4 + 3] = (u8)s->h[i];
	}
}

/* ------------------------------------------------------------------ */
/* Key derivation and helpers                                         */
/* ------------------------------------------------------------------ */

/* Salted, iterated digest of (type, credential). */
static void derive(const u8 *salt, int type, const char *cred, u8 out[32])
{
	struct sha256 s;
	u8 hdr[3];
	int i;

	hdr[0] = 'N';
	hdr[1] = (u8)type;
	hdr[2] = (u8)strlen(cred);

	sha_init(&s);
	sha_update(&s, hdr, sizeof(hdr));
	sha_update(&s, (const u8 *)cred, hdr[2]);
	sha_update(&s, salt, 16);
	sha_final(&s, out);

	for (i = 0; i < KDF_ROUNDS; i++) {
		sha_init(&s);
		sha_update(&s, out, 32);
		sha_update(&s, salt, 16);
		sha_final(&s, out);
	}
}

static int const_time_eq(const u8 *a, const u8 *b, u32 n)
{
	u8 diff = 0;
	u32 i;

	for (i = 0; i < n; i++)
		diff |= a[i] ^ b[i];

	return diff == 0;
}

static u32 entropy32(void)
{
	static u32 mix = 0x9e3779b9;
	u32 v = (u32)get_timer(0) * 2654435761u;

	v ^= gpt4_get_current_tick() * 40503u;
	v ^= (u32)(unsigned long)&v;
	mix = (mix ^ v) * 1664525u + 1013904223u;
	mix ^= mix >> 15;

	return mix;
}

int lockstore_cred_valid(int type, const char *cred)
{
	int n;
	int i;
	int seen[10];

	if (!cred)
		return 0;

	n = (int)strlen(cred);

	if (type == LOCK_PIN) {
		if (n < LOCK_PIN_MIN || n > LOCK_PIN_MAX)
			return 0;
		for (i = 0; i < n; i++)
			if (cred[i] < '0' || cred[i] > '9')
				return 0;
		return 1;
	}

	if (type == LOCK_PATTERN) {
		if (n < LOCK_PAT_MIN || n > LOCK_PAT_MAX)
			return 0;
		memset(seen, 0, sizeof(seen));
		for (i = 0; i < n; i++) {
			int d = cred[i] - '0';

			if (d < 0 || d > 8 || seen[d])
				return 0;
			seen[d] = 1;
		}
		return 1;
	}

	return 0;
}

/* ------------------------------------------------------------------ */
/* Raw storage                                                        */
/* ------------------------------------------------------------------ */

static u64 slot_off(int slot)
{
	return part_size - SLOT_AREA + (u64)slot * SECTOR;
}

static int rec_valid(const struct lock_rec *r)
{
	if (r->magic != REC_MAGIC || r->version != REC_VERSION)
		return 0;
	if (r->crc != crc32_calc((const u8 *)r, SECTOR - 4))
		return 0;
	if (r->enabled > 1)
		return 0;
	if (r->enabled && r->type != LOCK_PIN && r->type != LOCK_PATTERN)
		return 0;

	return 1;
}

static int sector_blank(const u8 *p)
{
	int i;

	for (i = 1; i < SECTOR; i++)
		if (p[i] != p[0])
			return 0;

	return p[0] == 0x00 || p[0] == 0xff;
}

/*
 * Prove that the end of the partition is free: valid LK image header in
 * front, then BLANK_GUARD bytes of erased flash directly before the slots.
 */
static int area_is_free(void)
{
	static u8 sec[SECTOR] __attribute__((aligned(64)));
	u32 magic;
	u32 img_size;
	u64 off;

	if (partition_read(NOVALK_LOCK_PART, 0, sec, SECTOR) != SECTOR)
		return 0;

	magic = *(u32 *)(void *)sec;
	img_size = *(u32 *)(void *)(sec + 4);
	if (magic != MTK_IMG_MAGIC)
		return 0;

	/* The image (payload + header) must end before the guard area. */
	if ((u64)img_size + SECTOR > part_size - SLOT_AREA - BLANK_GUARD)
		return 0;

	for (off = part_size - SLOT_AREA - BLANK_GUARD;
	     off < part_size - SLOT_AREA; off += SECTOR) {
		if (partition_read(NOVALK_LOCK_PART, (off_t)off, sec, SECTOR) !=
		    SECTOR)
			return 0;
		if (!sector_blank(sec))
			return 0;
	}

	return 1;
}

static int store_rec(const struct lock_rec *src)
{
	static u8 buf[SECTOR] __attribute__((aligned(64)));
	static u8 chk[SECTOR] __attribute__((aligned(64)));
	struct lock_rec r;
	int slot;

	if (!part_ok || !writable)
		return -1;

	memcpy(&r, src, sizeof(r));
	r.magic = REC_MAGIC;
	r.version = REC_VERSION;
	r.seq = cur.seq + 1;
	r.crc = crc32_calc((const u8 *)&r, SECTOR - 4);

	slot = (cur_slot == 0) ? 1 : 0;

	memcpy(buf, &r, SECTOR);
	if (partition_write(NOVALK_LOCK_PART, (off_t)slot_off(slot), buf,
			    SECTOR) != SECTOR)
		return -1;

	/* Read back: only trust the write if the data really is there. */
	if (partition_read(NOVALK_LOCK_PART, (off_t)slot_off(slot), chk,
			   SECTOR) != SECTOR)
		return -1;
	if (memcmp(buf, chk, SECTOR) != 0)
		return -1;

	memcpy(&cur, &r, sizeof(cur));
	cur_slot = slot;

	return 0;
}

/* ------------------------------------------------------------------ */
/* Public API                                                         */
/* ------------------------------------------------------------------ */

void lockstore_init(void)
{
	static u8 sec[SECTOR] __attribute__((aligned(64)));
	struct lock_rec a, b;
	int va = 0;
	int vb = 0;

	memset(&cur, 0, sizeof(cur));
	cur_slot = -1;
	part_ok = 0;
	writable = 0;

	if (partition_get_index_by_name(NOVALK_LOCK_PART) < 0)
		return;

	part_size = partition_get_size_by_name(NOVALK_LOCK_PART);
	if (part_size < MIN_PART_SIZE || (part_size % SECTOR) != 0)
		return;

	part_ok = 1;

	if (partition_read(NOVALK_LOCK_PART, (off_t)slot_off(0), sec, SECTOR) ==
	    SECTOR) {
		memcpy(&a, sec, SECTOR);
		va = rec_valid(&a);
	}
	if (partition_read(NOVALK_LOCK_PART, (off_t)slot_off(1), sec, SECTOR) ==
	    SECTOR) {
		memcpy(&b, sec, SECTOR);
		vb = rec_valid(&b);
	}

	if (va && (!vb || (int)(a.seq - b.seq) > 0)) {
		memcpy(&cur, &a, sizeof(cur));
		cur_slot = 0;
		writable = 1;	/* it is our area, we wrote it before */
	} else if (vb) {
		memcpy(&cur, &b, sizeof(cur));
		cur_slot = 1;
		writable = 1;
	} else {
		writable = area_is_free();
	}

	dprintf(INFO, "[LOCK] store: enabled %d type %d writable %d\n",
		cur.enabled, cur.type, writable);
}

int lockstore_enabled(void)
{
	return cur.enabled ? 1 : 0;
}

int lockstore_type(void)
{
	return cur.enabled ? cur.type : LOCK_NONE;
}

int lockstore_writable(void)
{
	return writable;
}

int lockstore_verify(const char *cred)
{
	u8 h[32];

	if (!cur.enabled || !cred)
		return 0;

	derive(cur.salt, cur.type, cred, h);

	return const_time_eq(h, cur.hash, 32);
}

int lockstore_set(int type, const char *cred)
{
	struct lock_rec n;
	int i;

	if (!lockstore_cred_valid(type, cred))
		return -1;

	memcpy(&n, &cur, sizeof(n));
	n.enabled = 1;
	n.type = (u8)type;
	n.fail_count = 0;
	for (i = 0; i < 16; i += 4) {
		u32 r = entropy32();

		memcpy(n.salt + i, &r, 4);
	}
	derive(n.salt, type, cred, n.hash);
	memset(n.pad, 0, sizeof(n.pad));

	return store_rec(&n);
}

int lockstore_clear(void)
{
	struct lock_rec n;

	memcpy(&n, &cur, sizeof(n));
	n.enabled = 0;
	n.type = 0;
	n.fail_count = 0;
	memset(n.salt, 0, sizeof(n.salt));
	memset(n.hash, 0, sizeof(n.hash));
	memset(n.pad, 0, sizeof(n.pad));

	return store_rec(&n);
}

u32 lockstore_fail_count(void)
{
	return cur.fail_count;
}

void lockstore_note_fail(void)
{
	struct lock_rec n;

	memcpy(&n, &cur, sizeof(n));
	n.fail_count++;
	/* Persist so a reboot cannot be used to reset the counter. */
	if (store_rec(&n) != 0)
		cur.fail_count++;
}

void lockstore_note_ok(void)
{
	struct lock_rec n;

	if (cur.fail_count == 0)
		return;

	memcpy(&n, &cur, sizeof(n));
	n.fail_count = 0;
	if (store_rec(&n) != 0)
		cur.fail_count = 0;
}
