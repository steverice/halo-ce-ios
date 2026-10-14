/* Cloud saves: which of the game's save slots go to the player's cloud storage and come back,
   and what happens when two devices disagree (docs/cloud-saves.md).

   A slot is one directory <save root>/u/UDATA/<12 hex digits>. It syncs as two units: the
   profile (SaveMeta.xbx with blam.sav, or a game type's blam.lst; record slot-<id>) and the saved
   game (savegame.bin, the Save and Quit checkpoint; record save-<id>-<key>). A saved game only
   loads in a build with the same game state layout and disc region, so its record is keyed by
   those: builds that cannot load each other's saved games keep separate records and never
   replace each other's. For each unit the engine compares three hashes: what is on disk, what
   the cloud holds, and the base (what this device last had in common with the cloud, from its
   state file), then uploads, downloads, or settles a conflict by keeping the further-along side
   and storing the other as a copy.

   Two rules hold whatever the store answers:
   1. Local bytes are not overwritten or deleted unless the cloud holds exactly those bytes (as
      the current record or a copy), or held them as current when this device last synced.
   2. The cloud's current record is not replaced by a device that did not have it as its base,
      except by the conflict procedure, which stores one side as a copy first.

   This file is the rules only: portable C, no threads, no cloud API. The store is a table of
   functions (CloudKit in port/ios/host/cloud_store_cloudkit.m, a fake in
   port/ios/tests/cloud_saves_probe.c). An engine is used from one thread at a time. */
#ifndef CLOUD_SAVES_H
#define CLOUD_SAVES_H

#include <stddef.h>
#include <stdint.h>

/* the layout of records and payloads this code writes, and the oldest reader that may use them */
#define CLOUD_FORMAT 1
#define CLOUD_MIN_FORMAT 1

enum
{
	CLOUD_SLOT_ID_SIZE = 13,          /* 12 hex digits */
	CLOUD_HASH_SIZE = 65,             /* SHA-256 in hex */
	CLOUD_NAME_SIZE = 65,             /* a file's name inside a slot */
	CLOUD_KEY_SIZE = 13,              /* a saved game's layout key, 12 hex digits */
	CLOUD_RECORD_NAME_SIZE = 48,
	CLOUD_TAG_SIZE = 64,
	CLOUD_META_SIZE = 1024,
	CLOUD_TEXT_SIZE = 64,
	CLOUD_MAX_FILES = 16,
	CLOUD_MAX_SLOTS = 64,
	CLOUD_MAX_COPIES = 5,             /* kept per unit, newest first */
	CLOUD_SETTLE_SECONDS = 2,         /* a slot's files must have been still this long to be read */
};
#define CLOUD_MAX_SLOT_BYTES ((size_t)64 << 20)

/* ---------- the store */

enum cloud_status
{
	CLOUD_OK,
	CLOUD_NOT_FOUND,      /* no such record */
	CLOUD_CHANGED,        /* a conditional save found another version (or, for "must not exist", a record) */
	CLOUD_RETRY,          /* no network, busy, rate limited: try again later */
	CLOUD_QUOTA,          /* the account's storage is full */
	CLOUD_NO_ZONE,        /* the zone was never made */
	CLOUD_ZONE_DELETED,   /* the player deleted the app's cloud data */
	CLOUD_NO_ACCOUNT,     /* signed out, restricted, or the app's cloud access is off */
	CLOUD_FAILED,         /* nothing a retry in this run fixes (entitlement, container) */
};

struct cloud_record
{
	char name[CLOUD_RECORD_NAME_SIZE];   /* slot-<id>, save-<id>-<key>, or copy-<id>-<profile|key>-<16 hex of the hash> */
	char unit[CLOUD_RECORD_NAME_SIZE];   /* the current record this is, or is a copy of */
	int copy;                            /* a set-aside copy, not a unit's current record */
	long long format, min_format;
	char hash[CLOUD_HASH_SIZE];
	char meta[CLOUD_META_SIZE];          /* key=value lines (cloud_meta_get) */
	char tag[CLOUD_TAG_SIZE];            /* the store's version of the record, opaque */
};

struct cloud_store
{
	void *context;
	/* writes an identifier of the signed-in account (any stable text) */
	enum cloud_status (*account)(void *context, char *identifier, size_t size);
	/* every record's fields, without payloads; *records is malloc'ed */
	enum cloud_status (*list)(void *context, struct cloud_record **records, int *count);
	enum cloud_status (*create_zone)(void *context);
	/* one record's payload, malloc'ed, and its fields as they are now */
	enum cloud_status (*fetch)(void *context, const char *name, struct cloud_record *record, uint8_t **payload,
		size_t *size);
	/* saves a record. expected_tag: "" when no record of that name may exist, else the tag the stored
	   record must still have. payload NULL keeps the stored payload (fields only). Writes the new tag. */
	enum cloud_status (*save)(void *context, const struct cloud_record *record, const uint8_t *payload, size_t size,
		const char *expected_tag, char *new_tag, size_t tag_size);
	enum cloud_status (*remove)(void *context, const char *name);
	enum cloud_status (*delete_zone)(void *context);
};

/* ---------- the engine */

struct cloud_config
{
	const char *save_root;     /* the game's save folder: slots are under <save_root>/u/UDATA */
	const char *state_path;    /* this device's bookkeeping file */
	const char *device;        /* a label for the player ("Apple Vision Pro") */
	const char *platform;
	const char *app;           /* version, for diagnosis */
	const char *image;         /* identifies the game build: a saved game written by the same image is known to load */
	long long state_size;      /* the size of this build's saved game file, 0 when unknown */
	int saved_game_version;    /* HALO_PORT_SAVED_GAME_VERSION: what this build's saved games mean, beyond their layout */
	int network_version;       /* for diagnosis */
	void *context;
	long long (*now)(void *context);                        /* seconds since 1970 */
	void (*log)(void *context, const char *line);
	/* brackets every change to the local slots; begin returns 0 when local slots may no longer
	   change (the game has started), and the change is then left for the next launch */
	int (*local_write_begin)(void *context);
	void (*local_write_end)(void *context);
	/* 16 random bytes, for the device's identifier */
	void (*random)(void *context, uint8_t *bytes, size_t size);
};

enum cloud_state
{
	CLOUD_STATE_IDLE,            /* in step with the cloud as of the last pass */
	CLOUD_STATE_PENDING,         /* something waits for the network or the next launch */
	CLOUD_STATE_NO_ACCOUNT,
	CLOUD_STATE_QUOTA,
	CLOUD_STATE_ACCOUNT_CHANGED, /* waiting for cloud_engine_answer */
	CLOUD_STATE_CLOUD_DELETED,   /* waiting for cloud_engine_answer */
	CLOUD_STATE_NEWER_FORMAT,    /* a slot was saved by a newer build and is left alone */
	CLOUD_STATE_FAILED,
};

enum cloud_notice_kind
{
	CLOUD_NOTICE_CONFLICT = 1,   /* two devices' versions differed: one kept, one set aside */
	CLOUD_NOTICE_QUOTA,
	CLOUD_NOTICE_ACCOUNT_CHANGED,
	CLOUD_NOTICE_CLOUD_DELETED,
	CLOUD_NOTICE_NEWER_FORMAT,
	CLOUD_NOTICE_RESTORE_INCOMPLETE,
};

/* what a slot holds, for the player and for choosing between two versions */
struct cloud_summary
{
	char player[CLOUD_TEXT_SIZE];      /* the profile's name, UTF-8 */
	char device[CLOUD_TEXT_SIZE];
	int profile;                       /* has a readable blam.sav */
	int levels;                        /* campaign levels finished, on any difficulty */
	int checkpoint;                    /* 1 when savegame.bin holds a valid saved game */
	int checkpoint_level;              /* 0-9 in campaign order, -1 when none or not a campaign map */
	char checkpoint_map[CLOUD_TEXT_SIZE];
	int difficulty;
	char game_build[CLOUD_TEXT_SIZE];  /* the disc's build the checkpoint is of */
	unsigned long state_layout;        /* the checkpoint's game state layout */
	long long state_size;
	long long modified;
};

struct cloud_notice
{
	enum cloud_notice_kind kind;
	char unit[CLOUD_RECORD_NAME_SIZE]; /* CONFLICT: slot-<id> (a profile) or save-<id>-<key> (a saved game) */
	int kept_local;                    /* CONFLICT: this device's version was kept */
	struct cloud_summary kept, aside;  /* CONFLICT */
};

struct cloud_engine;

struct cloud_engine *cloud_engine_create(const struct cloud_config *config, const struct cloud_store *store);
void cloud_engine_destroy(struct cloud_engine *engine);

enum cloud_pass
{
	CLOUD_PASS_RUNNING,   /* the game runs: uploads only, and only of files that have been still a while */
	CLOUD_PASS_LAUNCH,    /* the game has not started: local slots may be replaced (inside local_write_begin) */
	CLOUD_PASS_LEAVING,   /* the game has stopped writing (the app is leaving): uploads only, no wait for stillness */
};

/* One pass over every slot. Returns the state. */
enum cloud_state cloud_engine_sync(struct cloud_engine *engine, enum cloud_pass pass);
enum cloud_state cloud_engine_state(const struct cloud_engine *engine);
/* profiles and game types this device has in step with the cloud */
int cloud_engine_synced_slots(const struct cloud_engine *engine);
/* whether the device holds any slot at all (a new or purged device waits longer at launch) */
int cloud_local_slot_count(const char *save_root);

/* The game removed a slot's directory (an explicit delete, not a purge): written at once to a
   journal beside the state file, which the engine's next pass takes. Safe from any thread. */
void cloud_note_local_delete(const char *state_path, const char *slot);
/* A hash of the slots' file names, sizes and times (and of a deletion waiting in the journal):
   unchanged since a pass that ended idle means a running pass has nothing to send, so the host
   can skip it and the network request it starts with. */
void cloud_local_signature(const char *save_root, const char *state_path, char hash[CLOUD_HASH_SIZE]);
/* path is a slot's directory under save_root: writes its id and returns 1 */
int cloud_slot_of_path(const char *save_root, const char *path, char *slot);

/* notices for the player, oldest first; 0 when there are none. cloud_engine_notice_done drops the first. */
int cloud_engine_notice(const struct cloud_engine *engine, struct cloud_notice *notice);
void cloud_engine_notice_done(struct cloud_engine *engine);

/* ACCOUNT_CHANGED or CLOUD_DELETED: 1 to carry on with this device's saves (adding them to the
   new account, or uploading them again), 0 to stay out (the host turns the setting off) */
void cloud_engine_answer(struct cloud_engine *engine, int carry_on);

/* Before the game starts: makes a unit's newest set-aside copy (or a profile's deleted version)
   current again, setting the present one aside. unit NULL: whichever is newest of all.
   Returns CLOUD_OK, CLOUD_NOT_FOUND when there is none, or the store's failure. */
enum cloud_status cloud_engine_restore_copy(struct cloud_engine *engine, const char *unit);

/* Deletes everything in the cloud and forgets the bases; local slots stay. */
enum cloud_status cloud_engine_delete_cloud(struct cloud_engine *engine);

/* ---------- pieces, for the store and the tests */

/* a value from key=value lines; 0 when the key is absent */
int cloud_meta_get(const char *meta, const char *key, char *value, size_t size);
/* 12 hex digits */
int cloud_slot_id_valid(const char *id);
/* a plain file name: letters, digits, '.', '_', '-', at most 64, not "." or ".." */
int cloud_file_name_valid(const char *name);
void cloud_sha256_hex(const uint8_t *data, size_t size, char hex[CLOUD_HASH_SIZE]);
/* the game's own check of a saved game file (source/saved games/game_state_xbox.c): 1 valid,
   0 all zeros (a new profile's), -1 neither (torn or damaged) */
int cloud_checkpoint_check(const uint8_t *data, size_t size);
/* 1 when a is further along than b, -1 when b is, 0 when they cannot be told apart */
int cloud_summary_compare(const struct cloud_summary *a, const struct cloud_summary *b);
/* a level's title from its map name ("levels\\b30\\b30"), or the map name */
const char *cloud_level_title(const char *map);

#endif
