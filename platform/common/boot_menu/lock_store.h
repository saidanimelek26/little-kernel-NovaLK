/*
 * NovaLK boot menu lock: persistent credential storage.
 *
 * The credential (a PIN or an unlock pattern) is never stored.  Only a
 * salted, iterated SHA-256 digest is kept, together with the credential
 * type and a failed attempt counter.
 *
 * Where the data lives
 * --------------------
 * In the last two sectors of the "lk" partition, i.e. behind the end of the
 * bootloader image itself, so it survives reboots (and even LK updates that
 * do not erase the whole partition).  Two copies are written alternately,
 * each with a sequence number and a CRC, so an interrupted write can never
 * destroy the previous good record.
 *
 * Before the first write the code proves that the area is really free: the
 * image header must be valid and the last 16 KiB of the partition (outside
 * the record slots) must be blank.  If that cannot be shown the store is
 * reported as not writable and nothing is ever written.
 */
#ifndef NOVALK_LOCK_STORE_H
#define NOVALK_LOCK_STORE_H

#include <platform/mt_typedefs.h>

/* Partition that carries the records.  Change here to relocate them. */
#ifndef NOVALK_LOCK_PART
#define NOVALK_LOCK_PART	"lk"
#endif

#define LOCK_NONE	0
#define LOCK_PIN	1
#define LOCK_PATTERN	2

#define LOCK_PIN_MIN	4
#define LOCK_PIN_MAX	8
#define LOCK_PAT_MIN	4
#define LOCK_PAT_MAX	9

/* A credential is a string of digits '0'..'9' (pattern: node numbers 0..8). */

/* Load the stored state.  Never fails hard: on errors protection is off. */
void lockstore_init(void);

/* 1 when a credential is set and protection is active. */
int lockstore_enabled(void);

/* LOCK_PIN / LOCK_PATTERN while enabled, LOCK_NONE otherwise. */
int lockstore_type(void);

/* 1 when records can be written safely on this device. */
int lockstore_writable(void);

/* 1 if the credential matches the stored one (constant time compare). */
int lockstore_verify(const char *cred);

/* Store a new credential and enable protection.  0 on success. */
int lockstore_set(int type, const char *cred);

/* Disable protection and forget the credential.  0 on success. */
int lockstore_clear(void);

/* Failed attempts since the last successful unlock (persistent). */
u32 lockstore_fail_count(void);
void lockstore_note_fail(void);
void lockstore_note_ok(void);

/* Syntax check of a credential for a given type (1 = acceptable). */
int lockstore_cred_valid(int type, const char *cred);

#endif /* NOVALK_LOCK_STORE_H */
