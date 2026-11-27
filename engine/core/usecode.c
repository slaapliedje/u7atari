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
	int wait_kind;              /* the intrinsic waiting (0x0a, 0x0b, 0x0c) or 0 for a converse */
	int16_t number[4];          /* a number asked for: low, high, step, initial */
	char say[UC_SAY];
	int8_t book;                /* 2 a scroll, 3 a book being read (BookMode), else 0 */
	int16_t sign;               /* the sign's gump (DisplayRunes), its lines in say */
	int speaker, speaker_frame;
	int face[2], face_frame[2], speaking;      /* the two places faces are shown in */
} run;

static int avatar_work = -1;    /* the work SetWorkType gave the avatar (14: sleep), till NapTime */
static uint8_t faded;           /* FadePalette's: the screen faded out */
static void request(int function, int item, int event);
static void script_post(int ref, const struct Val *list, int delay);

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

/* The item a number is, in the world (not an NPC): 1. */
static int thing_of(int ref, struct EThing *t)
{
	return ref > 0 && ref < 0x4000 && e_ref_thing(ref, t);
}

/* What NPCs carry (held.c): their numbers from HELD_REF up. */
#define HELD_REF E_HELD_REF
#define NOWHERE (-32768)
static struct { int16_t function, item, event; uint8_t set; } pending;   /* PathRunUsecode's */
static int dragged = -1;        /* the item made or taken up, to be put somewhere (the original's
                                 * item being dragged), or -1 */

static int held_of(int ref)
{
	return ref >= HELD_REF && e_held_ok(ref - HELD_REF) ? ref - HELD_REF : -1;
}

static int ref_of_held(int i)
{
	return i < 0 ? 0 : HELD_REF + i;
}

/* Where ref puts what it is given (e_held_'s in: an NPC, a held container), or NOWHERE. */
static int in_of_ref(int ref)
{
	int k = npc_of_ref(ref), i = held_of(ref);

	struct EThing t;
	int d;

	if (k >= 0)
		return E_HELD_ON(npc[k].number);
	if (i >= 0)
		return i;
	/* a container in the world: its contents read in */
	if (thing_of(ref, &t) && e_shape_class(t.tf & 0x3ff) == 6 && (d = e_container_fill(&t)) >= 0)
		return E_HELD_WORLD(d);
	return NOWHERE;
}

/* The strength of the NPC that what is put in "in" comes to (0: none). */
static int strength_for(int in)
{
	int owner = E_HELD_IS_NPC(in) ? E_HELD_ON(0) - in : in >= 0 ? e_held_owner(in) : -1;

	for (int k = 0; k < nnpc && owner >= 0; k++)
		if (npc[k].number == owner)
			return npc[k].strength;
	return 0;
}

int e_uc_strength(int number)
{
	return strength_for(E_HELD_ON(number));
}

/* Where what ref refers to is (an NPC, the avatar where the world step has it; an item, a held
 * one where its NPC is): 1. */
static int place_of_ref(int ref, int *x, int *y, int *z)
{
	int k = npc_of_ref(ref);
	struct EThing t;

	if (held_of(ref) >= 0 && e_held_owner(held_of(ref)) >= 0)
		k = npc_of_ref(e_held_owner(held_of(ref)) == 0 ? UC_AVATAR : -e_held_owner(held_of(ref)));

	if (k < 0 && thing_of(ref, &t)) {
		*x = t.x;
		*y = t.y;
		*z = t.z;
		return 1;
	}
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
		kind_join(k, 1);
	}
}

int e_uc_avatar_male(void)
{
	int k = avatar_index();

	return k < 0 || npc[k].male;
}

int e_uc_party_number(int j)
{
	return j >= 0 && j < nparty ? npc[party[j]].number : -1;
}

int uc_party(int j)
{
	return j >= 0 && j < nparty ? party[j] : -1;
}

int e_uc_join(int number)
{
	for (int k = 0; k < nnpc; k++)
		if (npc[k].number == number) {
			party_add(k);
			return in_party(k);
		}
	return 0;
}

static void party_remove(int k)
{
	for (int j = 1; j < nparty; j++)
		if (party[j] == k) {
			memmove(&party[j], &party[j + 1], (size_t) (nparty - j - 1) * sizeof party[0]);
			nparty--;
			npc[k].status &= (uint16_t) ~0x0800;
			kind_join(k, 0);
		}
}

void uc_party_leave(int k)
{
	party_remove(k);
}

/* ---- the intrinsics ---- */

static uint8_t told[0x100];     /* intrinsics not done yet, told once */

static int arg_int(const struct Val *args, int n)  /* from 1 */
{
	return first_int(&args[-n]);
}

/* An intrinsic. 1 when it waits for the player (E_UC_ASK set; its result pushed on resuming). */
int e_uc_trace;

static int intrinsic(int num, struct Val *args, int argc, struct Val *ret)
{
	int k;

	*ret = val_empty();
	if (e_uc_trace) {
		char text[200];
		int at = snprintf(text, sizeof text, "uc: 0x%02x(", num);
		for (int a = 1; a <= argc && at < 150; a++) {
			const struct Val *v = &args[-a];
			if (v->len <= 1)
				at += snprintf(text + at, sizeof text - (size_t) at, "%s%d", a > 1 ? "," : "", arg_int(args, a));
			else
				for (int e = 0; e < v->len && at < 150; e++)
					at += snprintf(text + at, sizeof text - (size_t) at, "%s%s%d%s", a > 1 && e == 0 ? "," : "",
						e == 0 ? "[" : " ", atom(v, e)->n, e == v->len - 1 ? "]" : "");
		}
		snprintf(text + at, sizeof text - (size_t) at, ")\n");
		eb_log(text);
	}
	switch (num) {
	case 0x00:                  /* Random: 1 to n */
		k = arg_int(args, 1);
		*ret = val_int(k > 0 ? rnd(k) + 1 : 1);
		return 0;
	case 0x03: {                /* SetSpeaker: npc's face, frame */
		k = arg_int(args, 1);
		run.speaker = k == UC_AVATAR ? 0 : k < 0 ? -k : k;
		run.speaker_frame = arg_int(args, 2);
		/* its place: where it is shown already, else a free one, else the one not speaking */
		int place = run.face[0] == run.speaker ? 0 : run.face[1] == run.speaker ? 1
			: run.face[0] < 0 ? 0 : run.face[1] < 0 ? 1 : !run.speaking;
		run.face[place] = run.speaker;
		run.face_frame[place] = run.speaker_frame;
		run.speaking = place;
		return 0;
	}
	case 0x04:                  /* CloseSpeaker */
		k = arg_int(args, 1);
		k = k == UC_AVATAR ? 0 : k < 0 ? -k : k;
		for (int j = 0; j < 2; j++)
			if (run.face[j] == k)
				run.face[j] = -1;
		if (run.face[run.speaking] < 0)
			run.speaking = run.face[!run.speaking] >= 0 ? !run.speaking : 0;
		run.speaker = run.face[run.speaking];
		return 0;
	case 0x91:                  /* ResetConvFace: the first speaker back */
		if (run.face[0] >= 0)
			run.speaking = 0;
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
	case 0x1d:                  /* SetWorkType: npc, work (the avatar's: the bed NapTime gives, 14) */
		k = npc_of_ref(arg_int(args, 1));
		if (k >= 0 && k == avatar_index())
			avatar_work = arg_int(args, 2);
		else if (k >= 0)
			kind_period(k, arg_int(args, 2));
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
			uint8_t female = !npc[k].male;
			switch (arg_int(args, 2)) {
			case 0: p = &npc[k].strength; break;
			case 1: p = &npc[k].dexterity; break;
			case 2: p = &npc[k].intelligence; break;
			case 3: p = &npc[k].hit_points; break;
			case 4: p = &npc[k].combat; break;
			case 5: p = &npc[k].mana; break;
			case 6: p = &npc[k].magic; break;
			case 10: p = &female; break;
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
		*ret = val_int(k >= 0 && !npc[k].male);
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
	case 0x28: {                /* CountObjects: in an NPC, a container or the party; type,
	                             * quality, frame (UC_ALL: any) */
		int who = arg_int(args, 1), type = arg_int(args, 2), q = arg_int(args, 3), f = arg_int(args, 4);
		int n = 0;
		type = type == UC_ALL ? -1 : type;
		q = q == UC_ALL ? -1 : q;
		f = f == UC_ALL ? -1 : f;
		if (who == UC_PARTY)
			for (k = 0; k < nparty; k++)
				n += e_held_count(E_HELD_ON(npc[party[k]].number), type, q, f);
		else if (in_of_ref(who) != NOWHERE)
			n = e_held_count(in_of_ref(who), type, q, f);
		*ret = val_int(n);
		return 0;
	}
	case 0x2a: {                /* GetContItems: container, type, quality, frame */
		int in = in_of_ref(arg_int(args, 1)), type = arg_int(args, 2), q = arg_int(args, 3);
		int f = arg_int(args, 4), n = 0;
		int16_t refs[48];
		type = type == UC_ALL ? -1 : type;
		q = q == UC_ALL ? -1 : q;
		f = f == UC_ALL ? -1 : f;
		for (k = in == NOWHERE ? -1 : e_held_find(in, type, q, f, -1); k >= 0 && n < 48;
			k = e_held_find(in, type, q, f, k))
			refs[n++] = (int16_t) ref_of_held(k);
		struct Atom *t = val_list(ret, n);
		for (k = 0; t != NULL && k < n; k++) {
			t[k].type = A_INT;
			t[k].n = refs[k];
			t[k].s = NULL;
		}
		return 0;
	}
	case 0x2b: {                /* RemovePartyItems: count, type, quality, frame; 0 when the
	                             * party has too few */
		int count = arg_int(args, 1), type = arg_int(args, 2), q = arg_int(args, 3), f = arg_int(args, 4);
		int have = 0;
		q = q == UC_ALL ? -1 : q;
		f = f == UC_ALL ? -1 : f;
		for (k = 0; k < nparty; k++)
			have += e_held_count(E_HELD_ON(npc[party[k]].number), type, q, f);
		if (have < count) {
			*ret = val_int(0);
			return 0;
		}
		for (k = 0; k < nparty && count > 0; k++) {
			int in = E_HELD_ON(npc[party[k]].number), i;
			while (count > 0 && (i = e_held_find(in, type, q, f, -1)) >= 0) {
				int n = e_held_quantity(i);
				if (n <= count) {
					count -= n;
					e_held_remove(i);
				} else {
					e_held_set_quantity(i, n - count);
					count = 0;
				}
			}
		}
		*ret = val_int(1);
		return 0;
	}
	case 0x2c: {                /* AddPartyItems: count, type, quality, frame, temporary; the
	                             * members who took some (a stack of at most 100 at a time,
	                             * each to the first who can carry it) */
		int count = arg_int(args, 1), type = arg_int(args, 2) & 0x3ff, q = arg_int(args, 3);
		int f = arg_int(args, 4), member = 0, mask = 0, n = 0;
		int16_t refs[PARTY_MOST];
		f = f == UC_ALL ? 0 : f;
		q = q == UC_ALL ? 0 : q;
		if (count <= 0) {
			*ret = val_int(1);
			return 0;
		}
		if (f >= e_shape_frames((uint16_t) type))
			f = 0;
		while (count > 0 && member < nparty) {
			int counted = e_shape_class(type) == 3, amount = counted ? (count > 100 ? 100 : count) : 1;
			int i = e_held_add((uint16_t) (type | f << 10), (uint8_t) (counted ? 0x80 : q), E_HELD_LOOSE);
			if (i < 0)
				break;
			if (counted)
				e_held_set_quantity(i, amount);
			int in = E_HELD_ON(npc[party[member]].number);
			if (e_held_place(i, in, strength_for(in)) != 0) {
				e_held_remove(i);
				member++;
			} else {
				mask |= 1 << member;
				count -= amount;
			}
		}
		for (k = nparty - 1; k >= 0; k--)
			if (mask & 1 << k)
				refs[n++] = (int16_t) ref_of_npc(party[k]);
		if (n == 0) {
			*ret = val_int(0);
			return 0;
		}
		struct Atom *t = val_list(ret, n);
		for (k = 0; t != NULL && k < n; k++) {
			t[k].type = A_INT;
			t[k].n = refs[k];
			t[k].s = NULL;
		}
		return 0;
	}
	case 0x24:                  /* CreateNewObject: type; it is then the one to put somewhere */
		if (e_held_ok(dragged) && e_held_in(dragged) == E_HELD_LOOSE)
			e_held_remove(dragged);
		dragged = e_held_add((uint16_t) (arg_int(args, 1) & 0x3ff), 0, E_HELD_LOOSE);
		if (dragged >= 0 && e_shape_class(arg_int(args, 1) & 0x3ff) == 3)
			e_held_set_quality(dragged, 0x81);
		*ret = val_int(ref_of_held(dragged));
		return 0;
	case 0x25: {                /* SetLastCreated: an item taken up, to be put somewhere */
		struct EThing t;
		int i = held_of(arg_int(args, 1));
		*ret = val_int(0);
		if (e_held_ok(dragged) && e_held_in(dragged) == E_HELD_LOOSE && dragged != i)
			e_held_remove(dragged);
		dragged = -1;
		if (i >= 0) {
			e_held_place(i, E_HELD_LOOSE, 0);
			dragged = i;
		} else if (thing_of(arg_int(args, 1), &t)) {
			int q = e_thing_quality(&t);
			if (e_thing_remove(&t))
				dragged = e_held_add(t.tf, (uint8_t) (q < 0 ? 0 : q), E_HELD_LOOSE);
		}
		*ret = val_int(dragged >= 0);
		return 0;
	}
	case 0x26: {                /* PopToMap: the item taken up put at x, y, z (one value: gone) */
		const struct Val *where = &args[-1];
		struct EThing t;
		*ret = val_int(0);
		if (!e_held_ok(dragged) || e_held_in(dragged) != E_HELD_LOOSE)
			return 0;
		if (where->len == 3 && !e_thing_add(e_held_tf(dragged), (int16_t) to_int(atom(where, 0)),
				(int16_t) to_int(atom(where, 1)), (uint8_t) to_int(atom(where, 2)), &t))
			return 0;
		if (where->len != 1 && where->len != 3)
			return 0;
		e_held_remove(dragged);
		dragged = -1;
		*ret = val_int(1);
		return 0;
	}
	case 0x36: {                /* PopToNPC: the item taken up given to an NPC or container */
		int in = in_of_ref(arg_int(args, 1));
		*ret = val_int(0);
		if (!e_held_ok(dragged) || in == NOWHERE || e_held_place(dragged, in, strength_for(in)) != 0)
			return 0;
		if (e_shape_class(e_held_tf(dragged) & 0x3ff) == 3)
			e_held_set_quality(dragged, 0x80 | e_held_quantity(dragged));
		dragged = -1;
		*ret = val_int(1);
		return 0;
	}
	case 0x67:                  /* WearingFellowship: the medallion's frame 1 on the avatar */
		for (k = e_held_find(E_HELD_ON(0), 955, -1, 1, -1); k >= 0; k = e_held_find(E_HELD_ON(0), 955, -1, 1, k))
			if (e_held_in(k) == E_HELD_ON(0))
				break;
		*ret = val_int(k >= 0);
		return 0;
	case 0x33:                  /* ClickOnItem: what the player points at, [item, x, y, z] */
		run.wait_kind = num;
		run.waiting = E_UC_TARGET;
		return 1;
	case 0x0c:                  /* InputNumericValue: low, high, step, initial */
		for (k = 0; k < 4; k++)
			run.number[k] = (int16_t) arg_int(args, k + 1);
		run.wait_kind = num;
		run.waiting = E_UC_NUMBER;
		return 1;
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
	case 0x0d: {                /* SetItemShape: type, item */
		struct EThing t;
		int i = held_of(arg_int(args, 2));
		if (i >= 0)
			e_held_set_tf(i, (uint16_t) ((e_held_tf(i) & ~0x3ffu) | (arg_int(args, 1) & 0x3ff)));
		else if (thing_of(arg_int(args, 2), &t))
			e_thing_set(&t, (uint16_t) ((t.tf & ~0x3ffu) | (arg_int(args, 1) & 0x3ff)));
		return 0;
	}
	case 0x0e: {                /* FindNearest: item, type, distance (-1: 20) */
		int x, y, z, type = arg_int(args, 2), far = arg_int(args, 3);
		static struct EThing found[48];
		if (far == -1)
			far = 20;
		*ret = val_int(0);
		if (type < 0 || !place_of_ref(arg_int(args, 1), &x, &y, &z))
			return 0;
		int n = e_things((uint16_t) type, (int16_t) (x - far), (int16_t) (y - far), (int16_t) (x + far),
			(int16_t) (y + far), found, 48), best = -1, best_far = 0;
		for (k = 0; k < n; k++) {
			int d = far_of(x, y, found[k].x, found[k].y);
			if (best < 0 || d < best_far) {
				best = k;
				best_far = d;
			}
		}
		if (best >= 0)
			*ret = val_int(e_thing_ref(&found[best]));
		return 0;
	}
	case 0x0f:                  /* Sfx: at the avatar */
		e_sound(arg_int(args, 1), me_x, me_y);
		return 0;
	case 0x11:                  /* GetItemShape */
	case 0x12: {                /* GetItemFrame */
		struct EThing t;
		k = npc_of_ref(arg_int(args, 1));
		uint16_t tf = k >= 0 ? npc[k].tf : held_of(arg_int(args, 1)) >= 0 ? e_held_tf(held_of(arg_int(args, 1)))
			: thing_of(arg_int(args, 1), &t) ? t.tf : 0;
		*ret = val_int(num == 0x11 ? tf & 0x3ff : tf >> 10 & 31);
		return 0;
	}
	case 0x13: {                /* SetItemFrame: item, frame */
		struct EThing t;
		int i = held_of(arg_int(args, 1));
		if (i >= 0)
			e_held_set_tf(i, (uint16_t) ((e_held_tf(i) & ~(31u << 10)) | (uint16_t) (arg_int(args, 2) & 31) << 10));
		else if (thing_of(arg_int(args, 1), &t))
			e_thing_set(&t, (uint16_t) ((t.tf & ~(31u << 10)) | (uint16_t) (arg_int(args, 2) & 31) << 10));
		return 0;
	}
	case 0x14:                  /* GetQuality */
	case 0x16: {                /* GetItemQuantity (its quality byte, for those counted) */
		struct EThing t;
		int i = held_of(arg_int(args, 1));
		if (i >= 0) {
			*ret = val_int(num == 0x14 ? e_held_quality(i) : e_held_quantity(i));
			return 0;
		}
		k = thing_of(arg_int(args, 1), &t) ? e_thing_quality(&t) : 0;
		*ret = val_int(k < 0 ? 0 : num == 0x16 && k == 0 ? 1 : k);
		return 0;
	}
	case 0x15:                  /* SetQuality: item, quality; whether it has one */
		*ret = val_int(e_held_set_quality(held_of(arg_int(args, 1)), arg_int(args, 2)));
		return 0;
	case 0x17:                  /* SetQuantity: item, count; whether it has one */
		*ret = val_int(e_held_set_quantity(held_of(arg_int(args, 1)), arg_int(args, 2)));
		return 0;
	case 0x31:                  /* IsNpc */
		*ret = val_int(npc_of_ref(arg_int(args, 1)) >= 0);
		return 0;
	case 0x35: {                /* FindNearby: item or place, type, radius, flags */
		static struct EThing found[48];
		int x, y, z, type = arg_int(args, 2), r = arg_int(args, 3), n = 0;
		int16_t refs[48];
		const struct Val *where = &args[-1];
		if (where->len == 1) {
			if (!place_of_ref(first_int(where), &x, &y, &z))
				return 0;
		} else if (where->len >= 2) {
			x = to_int(atom(where, where->len == 4 ? 1 : 0));
			y = to_int(atom(where, where->len == 4 ? 2 : 1));
		} else
			return 0;
		if (type == UC_ALL || type < 0) {
			/* NPCs about (what the conversations look for) */
			for (k = 0; k < nnpc && n < 48; k++) {
				int nx, ny, nz;
				if (place_of_ref(ref_of_npc(k), &nx, &ny, &nz) && far_of(x, y, nx, ny) <= r)
					refs[n++] = (int16_t) ref_of_npc(k);
			}
		} else {
			int m = e_things((uint16_t) type, (int16_t) (x - r), (int16_t) (y - r), (int16_t) (x + r),
				(int16_t) (y + r), found, 48);
			for (k = 0; k < m; k++)
				refs[n++] = (int16_t) e_thing_ref(&found[k]);
			for (k = 0; k < nnpc && n < 48; k++)
				if ((npc[k].tf & 0x3ff) == type && far_of(x, y, npc[k].x, npc[k].y) <= r)
					refs[n++] = (int16_t) ref_of_npc(k);
		}
		struct Atom *t = val_list(ret, n);
		for (k = 0; t != NULL && k < n; k++) {
			t[k].type = A_INT;
			t[k].n = refs[k];
			t[k].s = NULL;
		}
		return 0;
	}
	case 0x40: {                /* ItemSay: item, text (over an NPC) */
		k = npc_of_ref(arg_int(args, 1));
		const struct Atom *a = atom(&args[-2], 0);
		if (k >= 0 && a->type == A_TEXT)
			e_bark(k == avatar_index() ? 0 : in_slot(k), a->s, 15 * TICK_BLANKS);
		return 0;
	}
	case 0x6f: {                /* RemoveItem */
		struct EThing t;
		int i = held_of(arg_int(args, 1));
		if (i >= 0) {
			e_held_remove(i);
			if (i == dragged)
				dragged = -1;
		} else if (thing_of(arg_int(args, 1), &t))
			e_thing_remove(&t);
		return 0;
	}
	case 0x86: {                /* PlaySoundEffect2: sound, item */
		int x, y, z;
		if (place_of_ref(arg_int(args, 2), &x, &y, &z))
			e_sound(arg_int(args, 1), (int16_t) x, (int16_t) y);
		return 0;
	}
	case 0x30: {                /* FindNearbyAvatar: the items of a type about the avatar */
		static struct EThing found[48];
		int16_t wx0, wy0, wx1, wy1;
		e_near_window(&wx0, &wy0, &wx1, &wy1);
		int n = arg_int(args, 1) < 0 ? 0 : e_things((uint16_t) arg_int(args, 1), wx0, wy0, wx1, wy1, found, 48);
		struct Atom *t = val_list(ret, n);
		for (k = 0; t != NULL && k < n; k++) {
			t[k].type = A_INT;
			t[k].n = (int16_t) e_thing_ref(&found[k]);
			t[k].s = NULL;
		}
		return 0;
	}
	case 0x3e: {                /* MoveObject: item, [x, y, z] (an NPC, a world item; not yet the
	                             * party) */
		const struct Val *to = &args[-2];
		struct EThing t, put;
		if (to->len < 3)
			return 0;
		int x = to_int(atom(to, 0)), y = to_int(atom(to, 1)), z = to_int(atom(to, 2));
		k = npc_of_ref(arg_int(args, 1));
		if (k >= 0 && k != avatar_index()) {
			npc[k].x = (int16_t) x;
			npc[k].y = (int16_t) y;
			npc[k].z = (uint8_t) z;
		} else if (thing_of(arg_int(args, 1), &t) && e_thing_remove(&t))
			e_thing_add(t.tf, (int16_t) x, (int16_t) y, (uint8_t) z, &put);
		return 0;
	}
	case 0x87: {                /* DirectionFrom: item, to an item or [x, y]: 0-7, north clockwise */
		int x, y, z, tx, ty, tz;
		const struct Val *to = &args[-2];
		*ret = val_int(0);
		if (!place_of_ref(arg_int(args, 1), &x, &y, &z))
			return 0;
		if (to->len >= 2) {
			tx = to_int(atom(to, 0));
			ty = to_int(atom(to, 1));
		} else if (!place_of_ref(first_int(to), &tx, &ty, &tz))
			return 0;
		int dx = tx - x, dy = ty - y, ax = dx < 0 ? -dx : dx, ay = dy < 0 ? -dy : dy;
		/* within half a step of a side: that side; else the corner between */
		int sx = ax * 2 < ay ? 0 : dx < 0 ? -1 : dx > 0, sy = ay * 2 < ax ? 0 : dy < 0 ? -1 : dy > 0;
		k = e_way_of(sx, sy);
		*ret = val_int(k < 0 ? 0 : k);
		return 0;
	}
	case 0x55:                  /* BookMode: what is said next is read in the item's book or scroll */
		k = held_of(arg_int(args, 1));
		{
			struct EThing t;
			int type = k >= 0 ? e_held_tf(k) & 0x3ff : thing_of(arg_int(args, 1), &t) ? t.tf & 0x3ff : 0;
			run.book = type == 797 ? 2 : 3;
		}
		return 0;
	case 0x32: {                /* DisplayRunes: gump, its lines: a sign shown till it is let go */
		const struct Val *lines = &args[-2];
		size_t at = 0;
		run.say[0] = 0;
		for (k = 0; k < lines->len && at + 2 < sizeof run.say; k++) {
			const struct Atom *a = atom(lines, k);
			if (a->type != A_TEXT)
				continue;
			int w = snprintf(run.say + at, sizeof run.say - at, "%s%s", at ? "\r" : "", a->s);
			at = w < 0 ? at : at + (size_t) w >= sizeof run.say ? sizeof run.say - 1 : at + (size_t) w;
		}
		run.sign = (int16_t) arg_int(args, 1);
		run.wait_kind = num;
		run.waiting = E_UC_SIGN;
		return 1;
	}
	case 0x62:                  /* IsPcInside */
		*ret = val_int(e_inside());
		return 0;
	case 0x69:                  /* GetSpeechTrack: none */
		*ret = val_int(-1);
		return 0;
	case 0x6e: {                /* GetContainer: what a held item is in (an NPC, a container) */
		int i = held_of(arg_int(args, 1)), in = e_held_in(i);
		*ret = val_int(i < 0 || in == E_HELD_LOOSE ? 0 : in >= 0 ? ref_of_held(in)
			: E_HELD_IS_WORLD(in) ? E_HELD_WORLD(0) - in + 1
			: E_HELD_ON(0) - in == 0 ? UC_AVATAR : -(E_HELD_ON(0) - in));
		return 0;
	}
	case 0x72: {                /* IsReadied: npc, slot, type, frame (held on it: slots not kept) */
		int in = in_of_ref(arg_int(args, 1)), type = arg_int(args, 3), f = arg_int(args, 4);
		*ret = val_int(0);
		for (k = in == NOWHERE ? -1 : e_held_find(in, type, -1, f == UC_ALL ? -1 : f, -1); k >= 0;
			k = e_held_find(in, type, -1, f == UC_ALL ? -1 : f, k))
			if (e_held_in(k) == in)
				*ret = val_int(1);
		return 0;
	}
	case 0x7d:                  /* PathRunUsecode: [x, y, z], function, item, event: the walk not
	                             * walked (the item used is at hand), the function run after */
		pending.function = (int16_t) arg_int(args, 2);
		pending.item = (int16_t) arg_int(args, 3);
		pending.event = (int16_t) arg_int(args, 4);
		pending.set = 1;
		*ret = val_int(1);
		return 0;
	case 0x85:                  /* IsNotBlocked: (not yet looked at: yes) */
		*ret = val_int(1);
		return 0;
	case 0x44:                  /* GetWeather: fair */
	case 0x58:                  /* GetBarge: none */
	case 0x74:                  /* StartSpeech: none played */
	case 0x79:                  /* InUsecode: none queued */
	case 0x81:                  /* InGumpMode */
	case 0x8e:                  /* InCombat */
	case 0x90:                  /* IsWater (not yet looked at) */
	case 0x48:                  /* DisplayMap (not yet) */
		*ret = val_int(0);
		return 0;
	case 0x41:                  /* SetToAttack, SetAttackMode, SetOppressor, AttackObject: no combat yet */
	case 0x4b:
	case 0x4c:
	case 0x54:
	case 0x4f:                  /* DisplayArea, WizardEye, SpriteEffect, SetOrrery, FlashMouse,
	                             * CloseGumps, SetLight, SetTimePalette, ViewTile: not yet */
	case 0x50:
	case 0x53:
	case 0x63:
	case 0x6a:
	case 0x7e:
	case 0x82:
	case 0x84:
	case 0x94:
	case 0x7a:                  /* CallGuards: no crimes yet */
		return 0;
	case 0x46:                  /* SitDown: npc, chair (the avatar's: sat in it; the party's not yet) */
		k = npc_of_ref(arg_int(args, 1));
		{
			struct EThing t;
			if (k >= 0 && k == avatar_index() && thing_of(arg_int(args, 2), &t))
				kind_avatar_sit(&t);
		}
		return 0;
	case 0x77: {                /* NapTime: the bed the avatar sleeps in: into it, then the bed's
	                             * sleep (0x622) run for it, as the original's sleep schedule does */
		struct EThing t;
		if (avatar_work == 14 && thing_of(arg_int(args, 1), &t) && kind_avatar_bed(&t))
			request(0x622, arg_int(args, 1), 1);
		avatar_work = -1;
		return 0;
	}
	case 0x8c:                  /* FadePalette: ticks, -, in */
		faded = !arg_int(args, 3);
		return 0;
	case 0x78:                  /* AdvanceTime: game ticks (1500 an hour) at once */
		e_world_advance(arg_int(args, 1) / 25);
		return 0;
	case 0x01:                  /* Post: item, script */
	case 0x02:                  /* PostInFuture: item, script, ticks */
		script_post(arg_int(args, 1), &args[-2], num == 0x02 ? arg_int(args, 3) : 0);
		return 0;
	case 0x2e:                  /* PlayMusic: track, item */
		e_music_play(arg_int(args, 1) & 255);
		*ret = val_int(0);
		return 0;
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

/* ---- scripts on items (the original's Post: its action queue's scripts), and the functions
 * asked to be run (by them, by the schedules) ---- */

#define SCRIPTS 16
#define SCRIPT_WORDS 24
static struct Script {
	int16_t ref;                /* the item (0: none) */
	int16_t delay;              /* ticks till its next step */
	uint8_t n, pc;
	int16_t op[SCRIPT_WORDS];
} scripts[SCRIPTS];
#define REQUESTS 8
static struct { int16_t function, item, event; } requests[REQUESTS];
static int nrequests;

static void request(int function, int item, int event)
{
	if (nrequests < REQUESTS) {
		requests[nrequests].function = (int16_t) function;
		requests[nrequests].item = (int16_t) item;
		requests[nrequests].event = (int16_t) event;
		nrequests++;
	}
}

void e_uc_request_function(int function, int item, int event)
{
	request(function, item, event);
}

int e_uc_request(int *function, int *item, int *event)
{
	if (nrequests == 0)
		return 0;
	*function = requests[0].function;
	*item = requests[0].item;
	*event = requests[0].event;
	memmove(&requests[0], &requests[1], (size_t) (--nrequests) * sizeof requests[0]);
	return 1;
}

int e_uc_faded(void)
{
	return faded;
}

static void script_post(int ref, const struct Val *list, int delay)
{
	struct Script *sc = NULL;

	for (int k = 0; k < SCRIPTS && sc == NULL; k++)
		if (scripts[k].ref == ref)
			sc = &scripts[k];   /* (its last one given up) */
	for (int k = 0; k < SCRIPTS && sc == NULL; k++)
		if (scripts[k].ref == 0)
			sc = &scripts[k];
	if (sc == NULL || ref == 0)
		return;
	sc->ref = (int16_t) ref;
	sc->delay = (int16_t) (delay > 0 ? delay : 0);
	sc->pc = 0;
	sc->n = 0;
	for (int k = 0; k < list->len && sc->n < SCRIPT_WORDS; k++)
		if (atom(list, k)->type == A_INT)
			sc->op[sc->n++] = atom(list, k)->n;
}

/* A script's item's frame set (an item in the world, a held one). */
static void script_frame(int ref, int frame, int step, int wrap)
{
	struct EThing t;
	int i = held_of(ref), tf, frames;

	if (i >= 0)
		tf = e_held_tf(i);
	else if (thing_of(ref, &t))
		tf = t.tf;
	else
		return;
	frames = e_shape_frames((uint16_t) (tf & 0x3ff));
	if (step != 0) {
		frame = (tf >> 10 & 31) + step;
		if (frame < 0 || frame >= frames)
			frame = !wrap ? (frame < 0 ? 0 : frames - 1) : frame < 0 ? frames - 1 : 0;
	}
	if (frame < 0 || frame >= frames || frame > 31)
		return;
	tf = (tf & ~(31 << 10)) | frame << 10;
	if (i >= 0)
		e_held_set_tf(i, (uint16_t) tf);
	else
		e_thing_set(&t, (uint16_t) tf);
}

/* The original's script steps (script.h's numbers): those the usecode posts that the engine
 * does; the rest passed over. */
void e_uc_scripts_step(void)
{
	for (int k = 0; k < SCRIPTS; k++) {
		struct Script *sc = &scripts[k];
		if (sc->ref == 0)
			continue;
		if (sc->delay > 0) {
			sc->delay--;
			continue;
		}
		while (sc->ref != 0 && sc->delay == 0) {
			if (sc->pc >= sc->n) {
				sc->ref = 0;
				break;
			}
			int op = sc->op[sc->pc++], arg = sc->pc < sc->n ? sc->op[sc->pc] : 0;
			switch (op) {
			case 39:            /* wait: ticks */
				sc->delay = (int16_t) (arg > 0 ? arg : 1);
				sc->pc++;
				break;
			case 85:            /* usecode: the function run for the item (event 2) */
				request(arg, sc->ref, 2);
				sc->pc++;
				sc->delay = 1;
				break;
			case 70:            /* frame */
				script_frame(sc->ref, arg, 0, 0);
				sc->pc++;
				break;
			case 77: case 78:   /* next frame (at the last: stays; wraps) */
				script_frame(sc->ref, 0, 1, op == 78);
				break;
			case 79: case 80:   /* previous frame */
				script_frame(sc->ref, 0, -1, op == 80);
				break;
			case 88: {          /* sound: its number */
				int x, y, z;
				if (place_of_ref(sc->ref, &x, &y, &z))
					e_sound(arg, (int16_t) x, (int16_t) y);
				sc->pc++;
				break;
			}
			case 45:            /* removed */
			case 44:            /* finished */
				sc->ref = 0;
				break;
			case 81: case 83: case 84: case 86: case 89: case 90:  /* (with an argument) */
				sc->pc++;
				break;
			default:            /* continue, no halt, the poses: passed over */
				break;
			}
		}
	}
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
				return run.waiting;
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
			if (e_uc_trace) {
				char text[48];
				snprintf(text, sizeof text, "uc: call 0x%04x item %d\n", id, item);
				eb_log(text);
			}
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

int e_uc_pending(int *function, int *item, int *event)
{
	if (!pending.set)
		return 0;
	pending.set = 0;
	*function = pending.function;
	*item = pending.item;
	*event = pending.event;
	return 1;
}

int e_uc_book(void)
{
	return run.on ? run.book : 0;
}

int e_uc_sign(void)
{
	return run.on && run.waiting == E_UC_SIGN ? run.sign : -1;
}

int e_uc_has(int function)
{
	return function >= 0 && function < UC_FUNCTIONS && e_uc_open() && uc_at[function] != 0;
}

int e_uc_start(int function, int item, int event)
{
	if (!e_uc_open() || code == NULL)
		return E_UC_DONE;
	if (run.on)
		end_run();
	pending.set = 0;
	for (int k = 0; k < nloaded; k++)
		loaded[loaded_list[k]] = 0;
	nloaded = 0;
	code_used = 0;
	heap_used = 0;
	heap_full = 0;
	memset(&run, 0, sizeof run);
	run.speaker = -1;
	run.face[0] = run.face[1] = -1;
	run.event = (int16_t) event;
	run.on = 1;
	if (!enter(body_of((uint16_t) function), item)) {
		run.on = 0;
		return E_UC_DONE;
	}
	return go();
}

int e_uc_number(int *low, int *high, int *step, int *initial)
{
	if (!run.on || run.waiting != E_UC_NUMBER)
		return 0;
	*low = run.number[0];
	*high = run.number[1];
	*step = run.number[2] > 0 ? run.number[2] : 1;
	*initial = run.number[3];
	return 1;
}

int e_uc_resume_number(int value)
{
	if (!run.on || run.waiting != E_UC_NUMBER)
		return E_UC_DONE;
	if (run.wait_kind & 0x200)
		push(val_int(value));
	run.waiting = 0;
	return go();
}

int e_uc_resume_target(int picked, int ref, int x, int y, int z)
{
	if (!run.on || run.waiting != E_UC_TARGET)
		return E_UC_DONE;
	if (run.wait_kind & 0x200) {
		struct Val v = val_int(0);
		if (picked) {
			struct Atom *t = val_list(&v, 4);
			const int16_t n[4] = { (int16_t) ref, (int16_t) x, (int16_t) y, (int16_t) z };
			for (int k = 0; k < 4 && v.len == 4; k++) {
				t[k].type = A_INT;
				t[k].n = n[k];
				t[k].s = NULL;
			}
		}
		push(v);
	}
	run.waiting = 0;
	return go();
}

int e_uc_resume(const char *answer)
{
	if (!run.on)
		return E_UC_DONE;
	if (run.waiting == E_UC_SAY)
		run.say[0] = 0;
	else if (run.waiting == E_UC_SIGN) {
		run.say[0] = 0;
		if (run.wait_kind & 0x200)
			push(val_int(0));
	}
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

int e_uc_face(int place, int *frame)
{
	if (place < 0 || place > 1)
		return -1;
	if (frame != NULL)
		*frame = run.face_frame[place];
	return run.face[place];
}

int e_uc_speaking(void)
{
	return run.face[run.speaking] >= 0 ? run.speaking : -1;
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

int e_uc_section(int k, struct ESection *s)
{
	switch (k) {
	case 0:
		memcpy(s->tag, "FLAG", 4);
		s->size = 1;
		s->count = s->most = sizeof flags;
		s->data = flags;
		return 1;
	case 1:
		memcpy(s->tag, "PRTY", 4);
		s->size = sizeof party[0];
		s->count = (uint32_t) nparty;
		s->most = PARTY_MOST;
		s->data = party;
		return 1;
	}
	return 0;
}

void e_uc_loaded(void)
{
	nparty = (int) e_save_count("PRTY");
	for (int j = 1; j < nparty; j++) {
		uint16_t tf = npc[party[j]].tf;
		kind_join(party[j], 1);
		npc[party[j]].tf = tf;  /* (as it was saved: walking, or standing) */
	}
	/* what was going on is not: no item being moved, no scripts, nothing asked for */
	dragged = -1;
	memset(scripts, 0, sizeof scripts);
	nrequests = 0;
	faded = 0;
	avatar_work = -1;
	pending.set = 0;
}
