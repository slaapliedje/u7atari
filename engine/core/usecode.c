/* The original's usecode (STATIC/USECODE), run as its interpreter runs it: what an NPC says when
 * spoken to, what an item does when used. See engine.h (e_uc_).
 *
 * The file is a run of functions, each its number, its size and its body: the body's text (the
 * strings its code points into), its argument and local counts, the numbers of the functions it
 * calls (by index), then its code. They are found by number once (an index of where each is) and
 * read into a code area as they are called, kept for the run.
 *
 * Values are as the original's: lists of atoms, each a number, a string or empty; a lone number
 * is a list of one. Arithmetic and comparison work on the first atoms, as the original's Node
 * does (strings and numbers mixed by its rules). A run keeps its values in an area of its own,
 * let go when it ends.
 *
 * The interpreter stops where the original waits for the player: each line said (E_UC_SAY), each
 * answer asked for (E_UC_ASK: a conversation's converse, and the intrinsics that ask). The backend
 * shows them and resumes it (e_uc_resume).
 *
 * NPCs are referred to as the usecode does: -n for NPC n, the avatar -356.
 */
#include <stdio.h>
#include <string.h>

#include "kinds.h"

#define UC_FUNCTIONS 4096
#define UC_CODE E_UC_CODE        /* the functions a run calls */
#define UC_HEAP E_UC_HEAP        /* the values a run makes */
#define UC_STACK 768
#define UC_FRAMES 48
#define UC_FLAGS 4096
#define UC_ANSWERS 40
#define UC_SAVED 8               /* answer lists put by (PushKeys) */
#define UC_SAY 2048

#define UC_AVATAR (-356)
#define UC_PARTY (-357)
#define UC_ALL (-359)

/* opcodes */
#define OP_FOREACH 0x02
#define OP_CONVERSE 0x04
#define OP_JUMP_IF_FALSE 0x05
#define OP_JUMP 0x06
#define OP_CASE 0x07
#define OP_ADD 0x09
#define OP_SUBTRACT 0x0a
#define OP_DIVIDE 0x0b
#define OP_MULTIPLY 0x0c
#define OP_MODULO 0x0d
#define OP_AND 0x0e
#define OP_OR 0x0f
#define OP_NOT 0x10
#define OP_POP_LOCAL 0x12
#define OP_PUSH_TRUE 0x13
#define OP_PUSH_FALSE 0x14
#define OP_GREATER 0x16
#define OP_LESS 0x17
#define OP_GREATER_EQUAL 0x18
#define OP_LESS_EQUAL 0x19
#define OP_NOT_EQUAL 0x1a
#define OP_SAY_TEXT 0x1c
#define OP_PUSH_TEXT 0x1d
#define OP_MAKE_LIST 0x1e
#define OP_PUSH_INT 0x1f
#define OP_PUSH_LOCAL 0x21
#define OP_EQUAL 0x22
#define OP_CALL 0x24
#define OP_RETURN 0x25
#define OP_PUSH_ELEMENT 0x26
#define OP_RETURN_ALT 0x2c
#define OP_RETURN_VALUE 0x2d
#define OP_FOREACH_START 0x2e
#define OP_SAY_LOCAL 0x2f
#define OP_IN 0x30
#define OP_DEFAULT 0x31
#define OP_RETURN_ZERO 0x32
#define OP_SAY 0x33
#define OP_CFUNCTION 0x38
#define OP_CROUTINE 0x39
#define OP_PUSH_ITEM 0x3e
#define OP_ABORT 0x3f
#define OP_ANSWERED 0x40
#define OP_PUSH_FLAG 0x42
#define OP_POP_FLAG 0x43
#define OP_PUSH_BYTE 0x44
#define OP_POP_ELEMENT 0x46
#define OP_CALL_ITEM 0x47
#define OP_PUSH_EVENT 0x48
#define OP_NULL_ROUTINE 0x49
#define OP_APPEND 0x4a
#define OP_POP_EVENT 0x4b

/* ---- values ---- */

#define A_EMPTY 0
#define A_INT 1
#define A_TEXT 2

struct Atom {
	uint8_t type;
	int16_t n;
	const char *s;
};

struct Val {
	uint16_t len;
	struct Atom one;            /* a list of one, here */
	struct Atom *a;             /* longer ones */
};

static uint8_t *heap;
static uint32_t heap_used;
static int heap_full;

static void *heap_take(uint32_t n)
{
	n = (n + 7) & ~7u;          /* (an atom holds a pointer) */
	if (heap_used + n > UC_HEAP) {
		heap_full = 1;
		return NULL;
	}
	void *p = heap + heap_used;
	heap_used += n;
	return p;
}

static const struct Atom empty_atom = { A_EMPTY, 0, NULL };

static const struct Atom *atom(const struct Val *v, int k)   /* from 0 */
{
	if (k < 0 || k >= v->len)
		return &empty_atom;
	return v->len == 1 ? &v->one : &v->a[k];
}

static struct Val val_int(int n)
{
	struct Val v = { 1, { A_INT, (int16_t) n, NULL }, NULL };
	return v;
}

static struct Val val_text(const char *s)
{
	struct Val v = { 1, { A_TEXT, 0, s }, NULL };
	return v;
}

static struct Val val_empty(void)
{
	struct Val v = { 0, { A_EMPTY, 0, NULL }, NULL };
	return v;
}

static struct Val val_atom(const struct Atom *a)
{
	struct Val v = { 1, *a, NULL };
	return v;
}

/* A list of n atoms (to be filled): its atoms. */
static struct Atom *val_list(struct Val *v, int n)
{
	v->len = (uint16_t) n;
	v->a = NULL;
	if (n <= 1)
		return &v->one;
	v->a = heap_take((uint32_t) n * sizeof *v->a);
	if (v->a == NULL)
		v->len = 0;
	return v->a;
}

static char *heap_text(const char *s, size_t n)
{
	char *t = heap_take((uint32_t) n + 1);
	if (t != NULL) {
		memcpy(t, s, n);
		t[n] = 0;
	}
	return t;
}

/* The original's ParseWholeNumber: the digits at the end of s (spaces after them let be). */
static int whole_number(const char *s, int *out)
{
	int i = (int) strlen(s) - 1, value = 0, scale = 1;

	while (i >= 0 && s[i] == ' ')
		i--;
	if (i < 0)
		return 0;
	int end = i;
	while (i >= 0 && s[i] >= '0' && s[i] <= '9') {
		value += (s[i] - '0') * scale;
		scale *= 10;
		i--;
	}
	if (i == end)
		return 0;
	if (i >= 0 && s[i] == '-')
		value = -value;
	*out = value;
	return 1;
}

static int to_int(const struct Atom *a)
{
	int n;

	if (a->type == A_INT)
		return a->n;
	if (a->type == A_TEXT && whole_number(a->s, &n))
		return n;
	return 0;
}

static int first_int(const struct Val *v)
{
	return to_int(atom(v, 0));
}

/* b op a, as the original's Node::calculate (b this, a other). */
static struct Atom calculate(int op, const struct Atom *b, const struct Atom *a)
{
	struct Atom r = { A_EMPTY, 0, NULL };
	char buf[8];
	int value;

	if (a->type == A_INT) {
		int n = a->n;
		if (b->type == A_INT) {
			r.type = A_INT;
			switch (op) {
			case OP_ADD: r.n = (int16_t) (b->n + n); break;
			case OP_SUBTRACT: r.n = (int16_t) (b->n - n); break;
			case OP_MULTIPLY: r.n = (int16_t) (b->n * n); break;
			case OP_DIVIDE: r.n = (int16_t) (n == 0 ? 0x7fff : b->n / n); break;
			case OP_MODULO: r.n = (int16_t) (n == 0 ? 0x7fff : b->n % n); break;
			}
		} else if (b->type == A_TEXT) {
			r = *b;
			if (op == OP_ADD) {
				snprintf(buf, sizeof buf, "%d", n);
				size_t lb = strlen(b->s);
				char *t = heap_take((uint32_t) (lb + strlen(buf) + 1));
				if (t != NULL) {
					memcpy(t, b->s, lb);
					strcpy(t + lb, buf);
					r.s = t;
				}
			}
		} else {
			r.type = A_INT;
			r.n = (int16_t) (op == OP_ADD ? n : op == OP_SUBTRACT ? -n : 0);
		}
		return r;
	}
	if (a->type == A_TEXT) {
		if (b->type == A_INT) {
			r.type = A_INT;
			r.n = b->n;
			if (whole_number(a->s, &value))
				switch (op) {
				case OP_ADD: r.n = (int16_t) (b->n + value); break;
				case OP_SUBTRACT: r.n = (int16_t) (b->n - value); break;
				case OP_MULTIPLY: r.n = (int16_t) (b->n * value); break;
				case OP_DIVIDE: r.n = (int16_t) (value == 0 ? 0x7fff : b->n / value); break;
				case OP_MODULO: r.n = (int16_t) (value == 0 ? 0x7fff : b->n % value); break;
				}
		} else if (b->type == A_TEXT) {
			r = *b;
			if (op == OP_ADD) {
				size_t lb = strlen(b->s), la = strlen(a->s);
				char *t = heap_take((uint32_t) (lb + la + 1));
				if (t != NULL) {
					memcpy(t, b->s, lb);
					memcpy(t + lb, a->s, la + 1);
					r.s = t;
				}
			}
		} else if (op == OP_ADD)
			r = *a;
		return r;
	}
	if (b->type != A_EMPTY)
		r = *b;
	return r;
}

/* b op a, as the original's Node::compare. */
static int compare(int op, const struct Atom *b, const struct Atom *a)
{
	char buf[8];
	int value, c = 0;

	if (a->type == A_INT) {
		int n = a->n;
		if (b->type == A_INT)
			c = b->n < n ? -1 : b->n > n;
		else if (b->type == A_TEXT) {
			snprintf(buf, sizeof buf, "%d", n);
			c = strcmp(b->s, buf);
			c = c < 0 ? -1 : c > 0;
		} else
			c = 0 < n ? -1 : 0 > n;  /* empty as 0 */
		switch (op) {
		case OP_EQUAL: return c == 0;
		case OP_GREATER: return c > 0;
		case OP_LESS: return c < 0;
		case OP_GREATER_EQUAL: return c >= 0;
		case OP_LESS_EQUAL: return c <= 0;
		case OP_NOT_EQUAL: return c != 0;
		}
		return 0;
	}
	if (a->type == A_TEXT) {
		int a_empty = a->s[0] == 0;
		if (b->type == A_INT) {
			if (!whole_number(a->s, &value))
				return op == OP_NOT_EQUAL;
			c = b->n < value ? -1 : b->n > value;
		} else if (b->type == A_TEXT) {
			c = strcmp(b->s, a->s);
			c = c < 0 ? -1 : c > 0;
		} else
			switch (op) {
			case OP_EQUAL: return a_empty;
			case OP_GREATER: return 0;
			case OP_LESS: return !a_empty;
			case OP_GREATER_EQUAL: return a_empty;
			case OP_LESS_EQUAL: return 1;
			case OP_NOT_EQUAL: return !a_empty;
			}
		switch (op) {
		case OP_EQUAL: return c == 0;
		case OP_GREATER: return c > 0;
		case OP_LESS: return c < 0;
		case OP_GREATER_EQUAL: return c >= 0;
		case OP_LESS_EQUAL: return c <= 0;
		case OP_NOT_EQUAL: return c != 0;
		}
		return 0;
	}
	/* other empty */
	if (b->type == A_INT)
		switch (op) {
		case OP_EQUAL: return b->n == 0;
		case OP_GREATER: return b->n > 0;
		case OP_LESS: return b->n < 0;
		case OP_GREATER_EQUAL: return b->n >= 0;
		case OP_LESS_EQUAL: return b->n <= 0;
		case OP_NOT_EQUAL: return b->n != 0;
		}
	if (b->type == A_TEXT)
		switch (op) {
		case OP_EQUAL: return b->s[0] == 0;
		case OP_GREATER: return b->s[0] != 0;
		case OP_LESS: return 0;
		case OP_GREATER_EQUAL: return 1;
		case OP_LESS_EQUAL: return b->s[0] == 0;
		case OP_NOT_EQUAL: return b->s[0] != 0;
		}
	return op == OP_EQUAL || op == OP_GREATER_EQUAL || op == OP_LESS_EQUAL;
}

static int contains(const struct Val *list, const struct Atom *a)
{
	for (int k = 0; k < list->len; k++)
		if (compare(OP_EQUAL, atom(list, k), a))
			return 1;
	return 0;
}

/* b's atoms then a's. */
static struct Val append(const struct Val *b, const struct Val *a)
{
	struct Val v;
	struct Atom *t = val_list(&v, b->len + a->len);

	if (t == NULL)
		return val_empty();
	for (int k = 0; k < b->len; k++)
		t[k] = *atom(b, k);
	for (int k = 0; k < a->len; k++)
		t[b->len + k] = *atom(a, k);
	return v;
}

/* v with its element index (from 1) made what's first in e, the rest of e after it (the
 * original's setElement), into a new list. */
static struct Val set_element(const struct Val *v, const struct Val *e, int index)
{
	int len = v->len, n;

	if (index < 1)
		return *v;
	if (index > len)
		len = index;
	n = len + (e->len > 1 ? e->len - 1 : 0);
	struct Val r;
	struct Atom *t = val_list(&r, n);
	if (t == NULL)
		return *v;
	int k = 0;
	for (int i = 1; i < index; i++)
		t[k++] = *atom(v, i - 1);
	t[k++] = *atom(e, 0);
	for (int i = 1; i < e->len; i++)
		t[k++] = *atom(e, i);
	for (int i = index + 1; i <= v->len; i++)
		t[k++] = *atom(v, i - 1);
	r.len = (uint16_t) k;
	return r;
}

/* ---- the file ---- */

static int16_t uc_file = -1;
static uint32_t uc_at[UC_FUNCTIONS];       /* each function's place in the file + 1, or 0 */
static uint8_t *code;
static uint32_t code_used;
static uint32_t loaded[UC_FUNCTIONS];      /* this run: its body's place in code + 1, or 0 */
static uint16_t loaded_list[512];
static int nloaded;
static uint8_t flags[UC_FLAGS / 8];

static uint16_t rd16(const uint8_t *p) { return (uint16_t) (p[0] | p[1] << 8); }

int e_uc_open(void)
{
	uint8_t head[4];
	int32_t at = 0, len;

	if (uc_file >= 0)
		return 1;
	uc_file = e_open_static("USECODE");
	if (uc_file < 0)
		return 0;
	len = plat_file_length(uc_file);
	while (at + 4 <= len) {
		if (plat_file_seek(uc_file, at, PLAT_SEEK_SET) != at || plat_file_read(uc_file, head, 4) != 4)
			break;
		uint16_t id = rd16(head), size = rd16(head + 2);
		if (id < UC_FUNCTIONS)
			uc_at[id] = (uint32_t) at + 1;
		at += 4 + size;
	}
	return 1;
}

/* Function id read in for this run: its body, or NULL. */
static const uint8_t *body_of(uint16_t id)
{
	uint8_t head[4];

	if (id >= UC_FUNCTIONS || uc_at[id] == 0)
		return NULL;
	if (loaded[id])
		return code + loaded[id] - 1;
	uint32_t at = uc_at[id] - 1;
	if (plat_file_seek(uc_file, (int32_t) at, PLAT_SEEK_SET) != (int32_t) at
		|| plat_file_read(uc_file, head, 4) != 4)
		return NULL;
	uint16_t size = rd16(head + 2);
	if (code_used + size + 2 > UC_CODE || nloaded == (int) (sizeof loaded_list / 2))
		return NULL;
	if (plat_file_read(uc_file, code + code_used, size) != size)
		return NULL;
	loaded[id] = code_used + 1;
	loaded_list[nloaded++] = id;
	code_used = (code_used + size + 1) & ~1u;
	return code + loaded[id] - 1;
}

/* ---- running ---- */

struct Frame {
	const uint8_t *body;        /* the function's body (its text first) */
	const uint8_t *pc;
	const uint8_t *externs;
	int16_t item;
	uint16_t base, nlocals, argc;
};

static struct Run {
	int on;
	struct Val stack[UC_STACK];
	int sp;
	struct Frame frames[UC_FRAMES];
	int depth;
	int16_t event;
	int answered;
	char answer[64];
	int waiting;                /* E_UC_ what it waits for */
	int wait_kind;              /* the intrinsic waiting (0x0a, 0x0b) or 0 for a converse */
	char say[UC_SAY];
	int speaker, speaker_frame;
} run;

static const char *answers[UC_ANSWERS];
static int nanswers;
static const char *saved[UC_SAVED][UC_ANSWERS];
static int nsaved[UC_SAVED], depth_saved;

static void answer_add(const char *s)
{
	for (int k = 0; k < nanswers; k++)
		if (!strcmp(answers[k], s))
			return;
	if (nanswers < UC_ANSWERS)
		answers[nanswers++] = s;
}

static void answer_remove(const char *s)
{
	for (int k = 0; k < nanswers; k++)
		if (!strcmp(answers[k], s)) {
			memmove(&answers[k], &answers[k + 1], (size_t) (nanswers - k - 1) * sizeof answers[0]);
			nanswers--;
			return;
		}
}

static void say_append(const char *s)
{
	size_t n = strlen(run.say), m = strlen(s);

	if (n + m >= sizeof run.say)
		m = sizeof run.say - 1 - n;
	memcpy(run.say + n, s, m);
	run.say[n + m] = 0;
}

/* A value as said: its atoms one after another. */
static void say_value(const struct Val *v)
{
	char buf[8];

	for (int k = 0; k < v->len; k++) {
		const struct Atom *a = atom(v, k);
		if (a->type == A_TEXT)
			say_append(a->s);
		else if (a->type == A_INT) {
			snprintf(buf, sizeof buf, "%d", a->n);
			say_append(buf);
		}
	}
}

static int flag_get(int n)
{
	return n >= 0 && n < UC_FLAGS ? flags[n >> 3] >> (7 - (n & 7)) & 1 : 0;
}

static void flag_set(int n, int on)
{
	if (n < 0 || n >= UC_FLAGS)
		return;
	if (on)
		flags[n >> 3] |= (uint8_t) (0x80 >> (n & 7));
	else
		flags[n >> 3] &= (uint8_t) ~(0x80 >> (n & 7));
}

/* ---- NPCs and the party, as the usecode refers to them ---- */

#define PARTY_MOST 9
static int16_t party[PARTY_MOST];  /* the party: NPC indices, the avatar first */
static int nparty;

static int npc_of_ref(int ref)
{
	int number = ref == UC_AVATAR ? 0 : ref < 0 ? -ref : -1;

	for (int k = 0; k < nnpc && number >= 0; k++)
		if (npc[k].number == number)
			return k;
	return -1;
}

static int ref_of_npc(int k)
{
	return k < 0 ? 0 : npc[k].number == 0 ? UC_AVATAR : -npc[k].number;
}

/* Where what ref refers to is (an NPC; the avatar where the world step has it): 1. */
static int place_of_ref(int ref, int *x, int *y, int *z)
{
	int k = npc_of_ref(ref);

	if (k < 0)
		return 0;
	if (k == avatar_index()) {
		*x = me_x;
		*y = me_y;
	} else {
		*x = npc[k].x;
		*y = npc[k].y;
	}
	*z = npc[k].z;
	return 1;
}

static int in_party(int k)
{
	for (int j = 0; j < nparty; j++)
		if (party[j] == k)
			return 1;
	return 0;
}

static void party_add(int k)
{
	if (k >= 0 && !in_party(k) && nparty < PARTY_MOST) {
		party[nparty++] = (int16_t) k;
		npc[k].status |= 0x0800;
	}
}

static void party_remove(int k)
{
	for (int j = 1; j < nparty; j++)
		if (party[j] == k) {
			memmove(&party[j], &party[j + 1], (size_t) (nparty - j - 1) * sizeof party[0]);
			nparty--;
			npc[k].status &= (uint16_t) ~0x0800;
		}
}

/* ---- the intrinsics ---- */

static uint8_t told[0x100];     /* intrinsics not done yet, told once */

static int arg_int(const struct Val *args, int n)  /* from 1 */
{
	return first_int(&args[-n]);
}

/* An intrinsic. 1 when it waits for the player (E_UC_ASK set; its result pushed on resuming). */
static int intrinsic(int num, struct Val *args, int argc, struct Val *ret)
{
	int k;

	*ret = val_empty();
	switch (num) {
	case 0x00:                  /* Random: 1 to n */
		k = arg_int(args, 1);
		*ret = val_int(k > 0 ? rnd(k) + 1 : 1);
		return 0;
	case 0x03:                  /* SetSpeaker: npc's face, frame */
		k = arg_int(args, 1);
		run.speaker = k == UC_AVATAR ? 0 : k < 0 ? -k : k;
		run.speaker_frame = arg_int(args, 2);
		return 0;
	case 0x04:                  /* CloseSpeaker */
		run.speaker = -1;
		return 0;
	case 0x05:                  /* TurnOn: answers offered */
	case 0x06:                  /* TurnOff: answers withdrawn */
		for (k = 0; k < args[-1].len; k++) {
			const struct Atom *a = atom(&args[-1], k);
			if (a->type != A_TEXT)
				continue;
			if (num == 0x05)
				answer_add(a->s);
			else
				answer_remove(a->s);
		}
		return 0;
	case 0x07:                  /* PushKeys */
		if (depth_saved < UC_SAVED) {
			memcpy(saved[depth_saved], answers, (size_t) nanswers * sizeof answers[0]);
			nsaved[depth_saved++] = nanswers;
		}
		nanswers = 0;
		return 0;
	case 0x08:                  /* PopKeys */
		if (depth_saved > 0) {
			depth_saved--;
			nanswers = nsaved[depth_saved];
			memcpy(answers, saved[depth_saved], (size_t) nanswers * sizeof answers[0]);
		}
		return 0;
	case 0x09:                  /* ClearKeys */
		nanswers = 0;
		return 0;
	case 0x0a:                  /* GetInput: an answer picked, among those in the list given */
	case 0x0b:                  /* GetOrdInput: ... its place */
		if (argc >= 1 && args[-1].len > 0) {
			/* the list given is offered alone, the answers as they were after */
			if (depth_saved < UC_SAVED) {
				memcpy(saved[depth_saved], answers, (size_t) nanswers * sizeof answers[0]);
				nsaved[depth_saved++] = nanswers;
			}
			nanswers = 0;
			for (k = 0; k < args[-1].len; k++)
				if (atom(&args[-1], k)->type == A_TEXT)
					answer_add(atom(&args[-1], k)->s);
			run.wait_kind = num | 0x100;
		} else
			run.wait_kind = num;
		run.waiting = E_UC_ASK;
		return 1;
	case 0x1b:                  /* GetNpcObject */
	case 0x3a:                  /* GetNpcNumber */
		k = arg_int(args, 1);
		*ret = val_int(k == 0 ? ref_of_npc(avatar_index()) : k);
		return 0;
	case 0x1c:                  /* GetWorkType */
		k = npc_of_ref(arg_int(args, 1));
		*ret = val_int(k >= 0 && doing[k].depth ? doing[k].level[0].kind : 0);
		return 0;
	case 0x1d:                  /* SetWorkType: work, npc */
		k = npc_of_ref(arg_int(args, 2));
		if (k >= 0)
			kind_period(k, arg_int(args, 1));
		return 0;
	case 0x1e:                  /* JoinParty */
		party_add(npc_of_ref(arg_int(args, 1)));
		return 0;
	case 0x1f:                  /* LeaveParty */
		party_remove(npc_of_ref(arg_int(args, 1)));
		return 0;
	case 0x20:                  /* GetNpcProp: npc, property */
		k = npc_of_ref(arg_int(args, 1));
		if (k >= 0) {
			static const uint8_t no = 0;
			const uint8_t *p = &no;
			switch (arg_int(args, 2)) {
			case 0: p = &npc[k].strength; break;
			case 1: p = &npc[k].dexterity; break;
			case 2: p = &npc[k].intelligence; break;
			case 3: p = &npc[k].hit_points; break;
			case 4: p = &npc[k].combat; break;
			case 5: p = &npc[k].mana; break;
			case 6: p = &npc[k].magic; break;
			}
			*ret = val_int(*p);
		}
		return 0;
	case 0x22:                  /* GetAvatarRef */
		*ret = val_int(UC_AVATAR);
		return 0;
	case 0x23:                  /* GetPartyList: the members, the avatar last */
	case 0x8d: {
		struct Atom *t = val_list(ret, nparty);
		for (k = 0; t != NULL && k < nparty; k++) {
			t[k].type = A_INT;
			t[k].n = (int16_t) ref_of_npc(party[nparty - 1 - k]);
			t[k].s = NULL;
		}
		return 0;
	}
	case 0x27: {                /* GetNPCName */
		struct Atom *t = val_list(ret, args[-1].len);
		for (k = 0; t != NULL && k < args[-1].len; k++) {
			int j = npc_of_ref(to_int(atom(&args[-1], k)));
			t[k].type = A_TEXT;
			t[k].n = 0;
			t[k].s = j >= 0 ? npc[j].name : "";
		}
		return 0;
	}
	case 0x38:                  /* GetHour */
		*ret = val_int(e_world_minutes() / 60);
		return 0;
	case 0x39:                  /* GameMinute */
		*ret = val_int(e_world_minutes() % 60);
		return 0;
	case 0x5a:                  /* AvatarSex: 1 for a woman */
		k = avatar_index();
		*ret = val_int(k >= 0 && (npc[k].tf & 0x3ff) != 721);
		return 0;
	case 0x5e:                  /* GetArraySize */
		*ret = val_int(args[-1].len);
		return 0;
	case 0x68:                  /* MouseExists */
		*ret = val_int(1);      /* (so Iolo does not tell the avatar to buy one) */
		return 0;
	case 0x88: {                /* GetItemFlag: item, flag */
		/* asleep, charmed, cursed, dead, 5, in the party, paralyzed, poisoned, protected */
		static const uint16_t bit[10] = { 0, 0x80, 0x100, 0x200, 0x8000, 0x400, 0x800, 0x1000,
			0x2000, 0x4000 };
		int f = arg_int(args, 2);
		k = npc_of_ref(arg_int(args, 1));
		if (f == 6)
			*ret = val_int(k >= 0 && in_party(k));
		else if (k >= 0 && f >= 1 && f <= 9)
			*ret = val_int((npc[k].status & bit[f]) != 0);
		else
			*ret = val_int(0);
		return 0;
	}
	case 0x10:                  /* DieRoll: low to high */
		k = arg_int(args, 2) - arg_int(args, 1) + 1;
		*ret = val_int(arg_int(args, 1) + (k > 0 ? rnd(k) : 0));
		return 0;
	case 0x18: {                /* GetCoord: x, y, z */
		int x, y, z;
		if (!place_of_ref(arg_int(args, 1), &x, &y, &z))
			return 0;
		struct Atom *t = val_list(ret, 3);
		if (t != NULL) {
			int c[3] = { x, y, z };
			for (k = 0; k < 3; k++) {
				t[k].type = A_INT;
				t[k].n = (int16_t) c[k];
				t[k].s = NULL;
			}
		}
		return 0;
	}
	case 0x19: {                /* GetDist: the greater of the deltas across and down */
		int x0, y0, z0, x1, y1, z1;
		*ret = val_int(place_of_ref(arg_int(args, 1), &x0, &y0, &z0)
			&& place_of_ref(arg_int(args, 2), &x1, &y1, &z1) ? far_of(x0, y0, x1, y1) : 0);
		return 0;
	}
	case 0x21:                  /* SetNpcProp: npc, property, by */
		k = npc_of_ref(arg_int(args, 1));
		if (k >= 0) {
			uint8_t *p = NULL;
			switch (arg_int(args, 2)) {
			case 0: p = &npc[k].strength; break;
			case 1: p = &npc[k].dexterity; break;
			case 2: p = &npc[k].intelligence; break;
			case 3: p = &npc[k].hit_points; break;
			case 4: p = &npc[k].combat; break;
			case 5: p = &npc[k].mana; break;
			case 6: p = &npc[k].magic; break;
			}
			if (p != NULL)
				*p = (uint8_t) (*p + arg_int(args, 3));
		}
		return 0;
	case 0x28:                  /* CountObjects (the avatar's belongings: not yet) */
	case 0x67:                  /* WearingFellowship (likewise) */
		*ret = val_int(0);
		return 0;
	case 0x2f: {                /* IsNPCNear: on the screen, and (an NPC) awake */
		int x, y, z;
		k = npc_of_ref(arg_int(args, 1));
		*ret = val_int(place_of_ref(arg_int(args, 1), &x, &y, &z) && x >= me_x - 20 && x < me_x + 20
			&& y >= me_y - 12 && y < me_y + 13 && (k < 0 || !(npc[k].status & 0x8080)));
		return 0;
	}
	case 0x37:                  /* IsDead */
		k = npc_of_ref(arg_int(args, 1));
		*ret = val_int(k >= 0 && (npc[k].status & 0x8000) != 0);
		return 0;
	case 0x3b:                  /* GetTime: the part of the day, an eighth */
		*ret = val_int(e_world_minutes() / 180);
		return 0;
	case 0x3c:                  /* GetAlignment */
		k = npc_of_ref(arg_int(args, 1));
		*ret = val_int(k >= 0 ? (npc[k].status & 0x18) >> 3 : 0);
		return 0;
	case 0x3d:                  /* SetAlignment */
		k = npc_of_ref(arg_int(args, 1));
		if (k >= 0)
			npc[k].status = (uint16_t) ((npc[k].status & ~0x18u) | ((arg_int(args, 2) << 3) & 0x18));
		return 0;
	case 0x89:                  /* SetItemFlag, ClearItemFlag: an NPC's (the status bits) */
	case 0x8a: {
		static const uint16_t bit[10] = { 0, 0x80, 0x100, 0x200, 0x8000, 0x400, 0x800, 0x1000,
			0x2000, 0x4000 };
		int f = arg_int(args, 2);
		k = npc_of_ref(arg_int(args, 1));
		if (k < 0)
			return 0;
		if (f == 6) {
			if (num == 0x89)
				party_add(k);
			else
				party_remove(k);
		} else if (f >= 1 && f <= 9) {
			if (num == 0x89)
				npc[k].status |= bit[f];
			else
				npc[k].status &= (uint16_t) ~bit[f];
		}
		return 0;
	}
	case 0x01:                  /* Post, PostInFuture: scripts on items (not yet: done) */
	case 0x02:
	case 0x2e:                  /* PlayMusic */
	case 0x5c:                  /* HaltScheduled */
		*ret = val_int(0);
		return 0;
	}
	if (!told[num & 0xff]) {
		char text[64];
		told[num & 0xff] = 1;
		snprintf(text, sizeof text, "usecode: intrinsic 0x%02x not done\n", num);
		eb_log(text);
	}
	*ret = val_int(0);
	return 0;
}

/* ---- the interpreter ---- */

static int16_t word(const uint8_t **pc)
{
	int16_t w = (int16_t) rd16(*pc);
	*pc += 2;
	return w;
}

static void push(struct Val v)
{
	if (run.sp < UC_STACK)
		run.stack[run.sp++] = v;
}

static struct Val pop_val(void)
{
	return run.sp > 0 ? run.stack[--run.sp] : val_empty();
}

/* Enters function body (called with item; nargs of its arguments on the stack). */
static int enter(const uint8_t *body, int item)
{
	if (body == NULL || run.depth == UC_FRAMES)
		return 0;
	struct Frame *f = &run.frames[run.depth++];
	const uint8_t *p = body;
	p += 2 + rd16(p);           /* past the text */
	uint16_t nargs = rd16(p), nlocals = rd16(p + 2), nexterns = rd16(p + 4);
	p += 6;
	f->body = body;
	f->externs = p;
	f->pc = p + nexterns * 2;
	f->item = (int16_t) item;
	f->base = (uint16_t) (run.sp >= nargs ? run.sp - nargs : 0);
	f->argc = nargs;
	f->nlocals = nlocals;
	for (int k = 0; k < nlocals; k++)
		push(val_empty());
	return 1;
}

static const char *text_at(const struct Frame *f, int offset)
{
	return (const char *) f->body + 2 + offset;
}

static void end_run(void)
{
	run.on = 0;
	run.waiting = E_UC_DONE;
	nanswers = 0;
	depth_saved = 0;
}

/* Runs on till the run ends or waits. */
static int go(void)
{
	struct Val a, b;
	int n1, n2;

	while (run.on && run.depth > 0) {
		struct Frame *f = &run.frames[run.depth - 1];
		const uint8_t *pc = f->pc;
		uint8_t op = *pc++;
#define LOCAL(n) run.stack[f->base + (n)]
		switch (op) {
		case OP_PUSH_EVENT:
			push(val_int(run.event));
			break;
		case OP_PUSH_ITEM:
			push(val_int(f->item));
			break;
		case OP_SAY_TEXT:
		case OP_SAY_LOCAL:
			while (op != OP_SAY) {
				n1 = word(&pc);
				if (op == OP_SAY_TEXT)
					say_append(text_at(f, n1));
				else
					say_value(&LOCAL(n1));
				op = *pc++;
			}
			f->pc = pc;
			run.waiting = E_UC_SAY;
			return E_UC_SAY;
		case OP_PUSH_TEXT:
			push(val_text(text_at(f, word(&pc))));
			break;
		case OP_PUSH_INT:
			push(val_int(word(&pc)));
			break;
		case OP_PUSH_BYTE:
			push(val_int(*pc++));
			break;
		case OP_PUSH_LOCAL:
			push(LOCAL(word(&pc)));
			break;
		case OP_PUSH_FLAG:
			push(val_int(flag_get(word(&pc))));
			break;
		case OP_ADD:
		case OP_SUBTRACT:
		case OP_DIVIDE:
		case OP_MULTIPLY:
		case OP_MODULO:
		case OP_APPEND:
			a = pop_val();
			b = pop_val();
			if (op == OP_APPEND)
				push(append(&b, &a));
			else {
				struct Atom r = calculate(op, atom(&b, 0), atom(&a, 0));
				push(val_atom(&r));
			}
			break;
		case OP_GREATER:
		case OP_LESS:
		case OP_GREATER_EQUAL:
		case OP_LESS_EQUAL:
		case OP_NOT_EQUAL:
		case OP_EQUAL:
			a = pop_val();
			b = pop_val();
			push(val_int(compare(op, atom(&b, 0), atom(&a, 0))));
			break;
		case OP_IN:
			a = pop_val();
			b = pop_val();
			push(val_int(contains(&a, atom(&b, 0))));
			break;
		case OP_AND:
			a = pop_val();
			b = pop_val();
			push(val_int(first_int(&a) && first_int(&b)));
			break;
		case OP_OR:
			a = pop_val();
			b = pop_val();
			push(val_int(first_int(&a) || first_int(&b)));
			break;
		case OP_NOT:
			a = pop_val();
			push(val_int(!first_int(&a)));
			break;
		case OP_PUSH_TRUE:
			push(val_int(1));
			break;
		case OP_PUSH_FALSE:
			push(val_int(0));
			break;
		case OP_DEFAULT:
			pc += 2;
			n1 = word(&pc);
			if (run.answered == 0)
				run.answered = 1;
			else
				pc += n1;
			break;
		case OP_CASE: {
			int found = 0;
			n1 = word(&pc);
			n2 = word(&pc);
			for (int k = 0; k < n1; k++) {
				a = pop_val();
				const struct Atom *t = atom(&a, 0);
				if (run.answered == 0 && t->type == A_TEXT && !strcmp(run.answer, t->s))
					found = run.answered = 1;
			}
			if (!found)
				pc += n2;
			break;
		}
		case OP_MAKE_LIST: {
			n1 = word(&pc);
			int total = 0;
			for (int k = 0; k < n1 && run.sp - 1 - k >= 0; k++)
				total += run.stack[run.sp - 1 - k].len;
			struct Val v;
			struct Atom *t = val_list(&v, total);
			int at = 0;
			/* the first popped first, as the original appends them */
			for (int k = 0; k < n1; k++) {
				b = pop_val();
				for (int j = 0; t != NULL && j < b.len; j++)
					t[at++] = *atom(&b, j);
			}
			push(t != NULL ? v : val_empty());
			break;
		}
		case OP_PUSH_ELEMENT:
			a = pop_val();
			n1 = word(&pc);
			push(val_atom(atom(&LOCAL(n1), first_int(&a) - 1)));
			break;
		case OP_JUMP_IF_FALSE:
			a = pop_val();
			n1 = word(&pc);
			if (first_int(&a) == 0)
				pc += n1;
			break;
		case OP_FOREACH_START: {
			pc++;               /* (the OP_FOREACH after it) */
			int counter = word(&pc), length = word(&pc);
			word(&pc);
			int list = word(&pc);
			LOCAL(counter) = val_int(1);
			LOCAL(length) = val_int(LOCAL(list).len);
			pc -= 9;            /* back to its OP_FOREACH */
			break;
		}
		case OP_FOREACH: {
			int counter = word(&pc), length = word(&pc), element = word(&pc), list = word(&pc);
			int c = first_int(&LOCAL(counter));
			n1 = word(&pc);
			if (c > first_int(&LOCAL(length))) {
				pc += n1;
				break;
			}
			LOCAL(element) = val_atom(atom(&LOCAL(list), c - 1));
			LOCAL(counter) = val_int(c + 1);
			break;
		}
		case OP_ANSWERED:
			run.answered = 1;
			break;
		case OP_CONVERSE:
			word(&pc);
			f->pc = pc;
			if (nanswers == 0) {
				end_run();
				return E_UC_DONE;
			}
			run.wait_kind = 0;
			run.waiting = E_UC_ASK;
			return E_UC_ASK;
		case OP_POP_LOCAL:
			a = pop_val();
			LOCAL(word(&pc)) = a;
			break;
		case OP_POP_EVENT:
			a = pop_val();
			run.event = (int16_t) first_int(&a);
			break;
		case OP_POP_ELEMENT:
			a = pop_val();
			b = pop_val();
			n1 = word(&pc);
			LOCAL(n1) = set_element(&LOCAL(n1), &b, first_int(&a));
			break;
		case OP_POP_FLAG:
			a = pop_val();
			flag_set(word(&pc), first_int(&a));
			break;
		case OP_JUMP:
			n1 = word(&pc);
			pc += n1;
			break;
		case OP_CFUNCTION:
		case OP_CROUTINE: {
			n1 = word(&pc);
			n2 = *pc++;
			struct Val r;
			f->pc = pc;
			if (intrinsic(n1, &run.stack[run.sp], n2, &r)) {
				/* waits: its arguments dropped and its result pushed on resuming */
				run.sp -= n2 < run.sp ? n2 : run.sp;
				run.wait_kind |= op == OP_CFUNCTION ? 0x200 : 0;
				return E_UC_ASK;
			}
			run.sp -= n2 < run.sp ? n2 : run.sp;
			if (op == OP_CFUNCTION)
				push(r);
			continue;           /* (f may have moved: the intrinsic may have run usecode) */
		}
		case OP_CALL:
		case OP_CALL_ITEM: {
			n1 = word(&pc);
			int item = f->item;
			uint16_t id = (uint16_t) n1;
			if (op == OP_CALL)
				id = rd16(f->externs + n1 * 2);
			else
				item = first_int(&run.stack[run.sp - 1]);
			f->pc = pc;
			if (!enter(body_of(id), item)) {
				char text[64];
				snprintf(text, sizeof text, "usecode: function 0x%04x not run\n", id);
				eb_log(text);
				end_run();
				return E_UC_DONE;
			}
			continue;
		}
		case OP_RETURN:
		case OP_RETURN_ALT:
		case OP_RETURN_VALUE:
		case OP_RETURN_ZERO:
			a = op == OP_RETURN_VALUE ? pop_val() : val_empty();
			run.sp = f->base;   /* its locals and arguments */
			run.depth--;
			if (op == OP_RETURN_VALUE)
				push(a);
			else if (op == OP_RETURN_ZERO)
				push(val_int(0));
			if (run.depth == 0) {
				end_run();
				return E_UC_DONE;
			}
			continue;
		case OP_NULL_ROUTINE:
		case OP_ABORT:
			end_run();
			return E_UC_DONE;
		default: {
			char text[64];
			snprintf(text, sizeof text, "usecode: opcode 0x%02x at %ld\n", op, (long) (pc - 1 - f->body));
			eb_log(text);
			end_run();
			return E_UC_DONE;
		}
		}
#undef LOCAL
		f->pc = pc;
		if (heap_full) {
			eb_log("usecode: out of value room\n");
			end_run();
			return E_UC_DONE;
		}
	}
	end_run();
	return E_UC_DONE;
}

int e_uc_start(int function, int item, int event)
{
	if (!e_uc_open() || code == NULL)
		return E_UC_DONE;
	if (run.on)
		end_run();
	for (int k = 0; k < nloaded; k++)
		loaded[loaded_list[k]] = 0;
	nloaded = 0;
	code_used = 0;
	heap_used = 0;
	heap_full = 0;
	memset(&run, 0, sizeof run);
	run.speaker = -1;
	run.event = (int16_t) event;
	run.on = 1;
	if (!enter(body_of((uint16_t) function), item)) {
		run.on = 0;
		return E_UC_DONE;
	}
	return go();
}

int e_uc_resume(const char *answer)
{
	if (!run.on)
		return E_UC_DONE;
	if (run.waiting == E_UC_SAY)
		run.say[0] = 0;
	else if (run.waiting == E_UC_ASK) {
		snprintf(run.answer, sizeof run.answer, "%s", answer != NULL ? answer : "");
		if (run.wait_kind == 0)
			run.answered = 0;   /* a converse: its cases look at it */
		else {
			int kind = run.wait_kind & 0xff, ord = 0;
			for (int k = 0; k < nanswers; k++)
				if (!strcmp(answers[k], run.answer))
					ord = k + 1;
			if (run.wait_kind & 0x100)
				intrinsic(0x08, NULL, 0, &(struct Val) { 0, { 0, 0, NULL }, NULL });  /* (PopKeys) */
			if (run.wait_kind & 0x200)
				push(kind == 0x0b ? val_int(ord)
					: val_text(heap_text(run.answer, strlen(run.answer))));
		}
	}
	run.waiting = 0;
	return go();
}

const char *e_uc_text(void)
{
	return run.say;
}

int e_uc_answers(const char **list, int most)
{
	int n = nanswers < most ? nanswers : most;

	for (int k = 0; k < n; k++)
		list[k] = answers[k];
	return n;
}

int e_uc_speaker(int *frame)
{
	if (frame != NULL)
		*frame = run.speaker_frame;
	return run.speaker;
}

int e_uc_flag(int n)
{
	return flag_get(n);
}

void e_uc_memory(void *code_room, void *heap_room)
{
	code = code_room;
	heap = heap_room;
}

void uc_world_init(void)
{
	nparty = 0;
	int k = avatar_index();
	if (k >= 0)
		party[nparty++] = (int16_t) k;
}
