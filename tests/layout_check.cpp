// Compile-time checks that game structs read from files keep the PC's byte layout on this
// host. Nothing runs: the file compiles or it doesn't.
//   m68k-atari-mint-g++ -std=gnu++20 -fsyntax-only -I<game>/include ... tests/layout_check.cpp
#include <stdint.h>
#include <string.h>

#pragma pack(1)
#include "byteorder.h"
#include "weapons.h"
#include "monsters.h"
#include "trigger.h"
#include "sche.h"

struct Bytes21 { uint8_t b[21]; };
struct Bytes25 { uint8_t b[25]; };
struct Bytes4 { uint8_t b[4]; };

static_assert(sizeof(WeaponRecord) == 21);
static_assert(sizeof(MonsterRecord) == 25);
static_assert(sizeof(ScheduleEntry) == 4);

// On the PC the first field of a bitfield byte is its lowest bit.
constexpr Bytes21 weapon_bytes()
{
	WeaponRecord r{};
	r.lucky = 1;          // byte 7 bit 0
	r.damageType = 0xa;   // byte 7 bits 4-7
	r.range = 3;          // byte 8 bits 3-7
	r.speed = 5;          // byte 10 bits 5-7
	r.noDamage = 1;       // byte 11 bit 7
	return __builtin_bit_cast(Bytes21, r);
}
static_assert(weapon_bytes().b[7] == 0xa1);
static_assert(weapon_bytes().b[8] == 3 << 3);
static_assert(weapon_bytes().b[10] == 5 << 5);
static_assert(weapon_bytes().b[11] == 0x80);

constexpr Bytes25 monster_bytes()
{
	MonsterRecord r{};
	r.sleepSafe = 1;      // byte 2 bit 0
	r.strength = 0x2a;    // byte 2 bits 2-7
	r.category = 5;       // byte 13 bits 0-2
	return __builtin_bit_cast(Bytes25, r);
}
static_assert(monster_bytes().b[2] == (0x2a << 2 | 1));
static_assert(monster_bytes().b[13] == 5);

constexpr Bytes4 schedule_bytes()
{
	ScheduleEntry e{};
	e.time = 6;           // bits 0-2
	e.type = 0x13;        // bits 3-7
	return __builtin_bit_cast(Bytes4, e);
}
static_assert(schedule_bytes().b[0] == (0x13 << 3 | 6));

// EggRecord's first two bytes are one little-endian word on the PC. EggRecord holds unions,
// which bit_cast cannot take at compile time, so this mirrors its two bitfield lines.
struct EggBits {
	U7_BITS(uint16_t, kind : 4, criteria : 3, nocturnal : 1);
	U7_BITS(uint16_t, once : 1, hatched : 1, distance : 5, repeat : 1);
};
struct Bytes2 { uint8_t b[2]; };
static_assert(sizeof(EggBits) == 2);
constexpr Bytes2 egg_bytes()
{
	EggBits e{};
	e.kind = 9;           // bits 0-3
	e.nocturnal = 1;      // bit 7
	e.once = 1;           // bit 8
	e.distance = 0x15;    // bits 10-14
	e.repeat = 1;         // bit 15
	return __builtin_bit_cast(Bytes2, e);
}
static_assert(egg_bytes().b[0] == (0x80 | 9));
static_assert(egg_bytes().b[1] == (0x80 | 0x15 << 2 | 1));
