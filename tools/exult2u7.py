#!/usr/bin/env python3
"""Converts an Exult saved game of Ultima VII, The Black Gate or Serpent Isle, into a saved game
for u7atari.
    tools/exult2u7.py [--title TEXT] <exultNNbg.sav | exultNNsi.sav | gamedat folder>
        <STATIC folder> <GAMEnn.U7>

The STATIC folder is the game's own (TFA.DAT and INITGAME.DAT are read from it), and says
which game it is. Copy the GAMEnn.U7 written (GAME00.U7 to GAME09.U7, slot 1 to 10) into the
game's folder and load it
from the game's menu: the game restarts and builds the imported game the way it builds a new
one, from the NPCs, the map's objects, the flags, the clocks and the party. Given an empty
folder instead of GAMEnn.U7, it writes the files there: as the game's GAMEDAT, the game starts
with the imported game.

Left out, as the original game has no place for them: monsters roaming the map (eggs make new
ones), Exult's scheduled usecode and object attributes, and what Exult added (notebook, the
Black Gate's keyring, shapes over 1023, other maps).
"""
import io, os, struct, sys, zipfile

NPC_RECORD = 117            # bytes of an NPC.DAT record before its inventory
SAVE_VERSION = 300
UNSET_TIMER = 43 * 1500     # what a timer holds until usecode sets it (hour 43)
AVATAR_SHAPE = 721

# ItemTypeClassFlags: what an item of each TFA class has (records in the low 2 bits)
CLASS_FLAGS = [0x0000, 0x0004, 0x0008, 0x0010, 0x0020, 0x0040, 0x00fe, 0x040a,
               0x0003, 0x0406, 0x0002, 0x000a, 0x01ee, 0x03ee, 0x0000, 0x0000]
CLASS_CONTENTS = 0x80
CLASS_EGG, CLASS_SPELLBOOK, CLASS_BARGE, CLASS_VIRTUE_STONE = 7, 8, 9, 11
KIND_CONTAINED = 6          # placement byte: kind in bits 0-2, lift in 4-7

warnings = []


def warn(text):
    if text not in warnings:
        warnings.append(text)


def read_flex(data):
    """The entries of a Flex file (13-byte name, then data) by lower-case name."""
    count = struct.unpack_from('<i', data, 84)[0]
    files = {}
    for i in range(count):
        offset, size = struct.unpack_from('<ii', data, 128 + 8 * i)
        if size >= 13:
            name = data[offset:offset + 13].split(b'\0')[0].decode('latin-1').lower()
            files[name.rstrip('.')] = data[offset + 13:offset + size]
    return files


def read_exult_save(path):
    """An Exult game's title and its files by lower-case name, from a gamedat folder or a .sav
    file (an 80-byte title, then a ZIP or a Flex)."""
    if os.path.isdir(path):
        return '', {n.lower(): open(os.path.join(path, n), 'rb').read()
                    for n in os.listdir(path) if os.path.isfile(os.path.join(path, n))}
    data = open(path, 'rb').read()
    title = data[:80].split(b'\0')[0].decode('latin-1')
    if data[80:84] == b'PK\3\4':
        files = {}
        with zipfile.ZipFile(io.BytesIO(data)) as z:
            for name in z.namelist():
                if '/' in name.rstrip('/'):
                    if not name.endswith('/'):
                        warn(f'{name}: other maps are left out')
                    continue
                files[name.lower()] = z.read(name)
        return title, files
    return title, read_flex(data)


def find_file(folder, name):
    for n in os.listdir(folder):
        if n.lower() == name.lower():
            return os.path.join(folder, n)
    sys.exit(f'exult2u7: no {name} in {folder}')


class Converter:
    def __init__(self, static):
        tfa = open(find_file(static, 'TFA.DAT'), 'rb').read()
        self.classes = [tfa[i * 3 + 1] & 15 for i in range(len(tfa) // 3)]
        self.initgame = read_flex(open(find_file(static, 'INITGAME.DAT'), 'rb').read())
        # Serpent Isle's new game has PHILBERT.DAT (the keyring and such)
        self.serpent_isle = 'philbert.dat' in self.initgame
        self.timer_count = 13 if self.serpent_isle else 12

    def type_class(self, shape):
        return self.classes[shape] if shape < len(self.classes) else 0

    # Exult's IREG lists

    def parse_list(self, buf, p, nested):
        """Reads an Exult object list from buf at p: to its 0 or 1 when nested, else to the end
        of buf. Returns (objects, p). Each object is a dict: the entry bytes (with an extended
        entry's 2-byte shape folded back), its shape and frame, which extended form it came in,
        its spot in an NPC's inventory and what it holds."""
        objects = []
        spot = -1
        while p < len(buf):
            length = buf[p]
            p += 1
            if length in (0, 1):
                if nested:
                    return objects, p
                continue                        # the end of a chunk's list
            if length == 2:                     # the inventory spot of the next object
                spot = struct.unpack_from('<b', buf, p)[0]
                p += 2
                continue
            if length == 255:                   # scheduled usecode, attributes, a name
                kind = buf[p]
                p += 1
                if kind != 2:
                    p += 2 + struct.unpack_from('<H', buf, p)[0]
                    warn('scheduled usecode and object attributes are left out')
                continue
            extended = 0
            if length in (253, 254):
                extended = length
                length = buf[p]
                p += 1
            entry = bytearray(buf[p:p + length])
            p += length
            if extended == 254:                 # 2-byte shape, then the frame
                shape, frame = entry[2] | entry[3] << 8, entry[4]
                del entry[4]
            else:
                shape, frame = entry[2] | (entry[3] & 3) << 8, entry[3] >> 2
            obj = {'entry': entry, 'shape': shape, 'frame': frame, 'extended': extended,
                   'spot': spot, 'contents': []}
            spot = -1
            kind = self.type_class(shape)
            # what Exult reads into an object: a container's, a body's or a barge's list
            if len(entry) in (12, 13) and (entry[4] or entry[5]) \
                    and kind not in (CLASS_EGG, CLASS_VIRTUE_STONE):
                obj['contents'], p = self.parse_list(buf, p, True)
            objects.append(obj)
        if nested:
            raise ValueError('an object list runs off the end')
        return objects, p

    def lift(self, obj):
        """The object's lift, from wherever its kind of entry keeps it."""
        entry = obj['entry']
        at = {6: 4, 10: 4, 12: 9, 13: 10, 14: 9, 18: 9}.get(len(entry))
        if at is None:
            return 0
        value = (entry[at] >> 4 | entry[at] << 4) & 0xff
        if obj['extended'] != 253:
            value &= 15
        if value > 15:
            warn('objects above lift 15 are put at lift 15')
            value = 15
        return value

    def write_object(self, obj, region, contained, out):
        """Appends obj's entry, and what it holds, in the original's format."""
        shape, frame, entry = obj['shape'], obj['frame'], obj['entry']
        if self.serpent_isle and shape == 975 and frame == 0:
            # Serpent Isle's new game gives Morghrim shape 975 for 792, and the game turns
            # each 975 frame 0 it loads after him into 792; Exult keeps the 975
            shape = 792
        if shape > 1023 or frame > 63:
            warn(f'shapes over 1023 and frames over 63 are left out (shape {shape})')
            return
        kind = self.type_class(shape)
        flags = CLASS_FLAGS[kind]
        records = max(1, flags & 3)
        place = self.lift(obj) << 4 | (KIND_CONTAINED if contained else 0)
        head = bytes([entry[0], entry[1], shape & 0xff, (shape >> 8) | frame << 2])
        contents = []
        if records == 1:
            quality = entry[5] if len(entry) in (6, 10) else entry[7]
            data = head + bytes([place, quality])
        elif records == 3:                      # spellbook
            data = bytearray(head + entry[4:18].ljust(14, b'\0'))
            data[9] = place
        elif flags & CLASS_CONTENTS:            # container or body
            if len(entry) in (6, 10):
                quality, npc, hits, quality_flags = entry[5], 0, 0, 0
            elif len(entry) == 13:              # Exult's body: a 2-byte NPC number
                quality, hits, quality_flags = entry[7], entry[11], entry[12]
                number = entry[8] | entry[9] << 8
                npc = 0x80 + number if quality == 1 and 0 < number < 128 else 0
            else:
                quality, npc, hits, quality_flags = entry[7], entry[8], entry[10], entry[11]
            contents = obj['contents']
            data = head + bytes([1 if contents else 0, 0, region, quality, npc, place, hits,
                                 quality_flags])
        elif len(entry) >= 12:                  # egg, barge, virtue stone
            data = bytearray(head + entry[4:12])
            data[9] = place
            if kind == CLASS_VIRTUE_STONE:
                data[10] = 0                    # Exult keeps its map here
        else:
            data = head + bytes([0, 0, region, entry[5], 0, place, 0, 0])
        if obj['contents'] and not contents:
            warn(f'what shape {shape} holds is left out: the original has it hold nothing')
        out.append(len(data))
        out.extend(data)
        if contents:
            for child in contents:
                self.write_object(child, region, True, out)
            out.append(1)

    def convert_region(self, buf, region):
        """An Exult U7IREGxx file as the original's: a list for each chunk, row by row."""
        objects, _ = self.parse_list(buf, 0, False)
        chunks = [[] for _ in range(256)]

        def place(obj):
            entry = obj['entry']
            chunks[(entry[1] >> 4) * 16 + (entry[0] >> 4)].append(obj)
            if self.type_class(obj['shape']) == CLASS_BARGE:
                # Exult nests a barge's parts in it; the original lists them on the map
                for part in obj['contents']:
                    place(part)
                obj['contents'] = []

        for obj in objects:
            place(obj)
        out = bytearray()
        for chunk in chunks:
            for obj in chunk:
                self.write_object(obj, region, False, out)
            out.append(0)
        return bytes(out)

    # NPC.DAT

    def parse_npcs(self, buf, p, count):
        """count Exult NPC records from buf at p: (record, inventory) pairs, and the new p."""
        npcs = []
        for _ in range(count):
            record = bytearray(buf[p:p + 78])
            p += 78
            iflags = record[4] | record[5] << 8
            if iflags & 8:                      # a usecode function name
                p += 1 + buf[p]
            record += buf[p:p + NPC_RECORD - 78]
            p += NPC_RECORD - 78
            inventory = []
            if iflags & 1:
                inventory, p = self.parse_list(buf, p, True)
            if iflags & 2:                      # scheduled usecode, to an end mark
                while buf[p] == 255:
                    kind = buf[p + 1]
                    p += 2
                    if kind == 2:
                        break
                    p += 2 + struct.unpack_from('<H', buf, p)[0]
            npcs.append((record, inventory))
        return npcs, p

    def convert_npc(self, number, record, inventory, base):
        """The original's NPC.DAT record for Exult's, with the inventory after it. Fields Exult
        uses for its own ends, or doesn't keep, come from the new game's record (base)."""
        out = bytearray(base)
        nb = 12                                 # where the NPC buffer starts
        flags = struct.unpack_from('<I', record, 68)[0]
        shape = struct.unpack_from('<H', record, 64)[0] or (record[2] | (record[3] & 3) << 8)
        frame = record[3] >> 2
        if number == 0:
            # the original's avatar is always shape 721, drawn as 989 for a woman; Exult
            # gives a woman 989, and other skins shapes of its own
            shape = AVATAR_SHAPE
        elif shape > 1023:
            warn(f'NPC {number}: shape {shape} is over 1023; kept its old one')
            shape = base[2] | (base[3] & 3) << 8
        used = struct.unpack_from('<H', record, 14)[0] != 0
        out[0:2] = record[0:2]
        out[2:4] = struct.pack('<H', shape | frame << 10)
        out[4:6] = struct.pack('<H', 1 if inventory and used else 0)
        out[6] = record[6]                      # superchunk
        out[9] = (record[9] & 0xf0) | (base[9] & 0x0f)      # lift; placement kind
        out[10] = record[10]                    # hit points
        out[11] = (base[11] & ~1) | (flags & 1)             # invisible
        out[nb + 2:nb + 4] = struct.pack('<H', 1 if used else 0)
        # Exult's status bits are the original's, but for 0x400 (Exult sets it on the avatar
        # on foot too) and 0x40 (Exult's "temporary"); facing and first alignment stay
        status = struct.unpack_from('<H', record, 16)[0]
        status = (status & 0x7b98) | (struct.unpack_from('<H', base, 16)[0] & 0x67)
        if flags & 1 << 4:                      # dead
            status |= 0x8000
        out[nb + 4:nb + 6] = struct.pack('<H', status)
        strength = record[18]
        if self.serpent_isle:
            # strength in bits 0-4, the avatar's skin colour in 5-6, frozen in 7
            if number == 0:
                skin = record[78] if struct.unpack_from('<H', record, 4)[0] & 16 \
                    else strength >> 5 & 3
                if skin > 2:
                    warn(f'the avatar\'s skin colour {skin} is Exult\'s own; made the first')
                    skin = 0
                strength = (strength & 0x9f) | skin << 5
            out[nb + 6] = strength
        else:
            out[nb + 6] = strength if number == 0 else (strength & 0x3f) | (base[18] & 0xc0)
        out[nb + 7] = record[19]                # dexterity
        out[nb + 8] = record[20] & 0x7f         # intelligence, read, tournament
        out[nb + 9] = record[21]                # combat, petra
        activity = record[22]
        if activity >= 32:                      # one of Exult's own
            activity = record[58] if record[58] < 32 else base[22]
        out[nb + 10] = activity
        out[nb + 11] = record[23] & 0x0f        # attack mode
        out[nb + 12:nb + 14] = out[2:4]         # type, set again on loading
        if record[25] & 0x80:
            # Exult's own way: magic, mana, temperature, then met, no spellcasting, zombie and
            # the id in one byte. The original packs the temperature into the top 3 bits of
            # two bytes: the avatar's magic and mana, or another's id and those flags.
            magic, mana, temperature, flags3 = record[25] & 0x7f, record[26], record[27], record[28]
            low = (magic, mana) if number == 0 else (flags3 >> 3, flags3 & 7)
            out[nb + 15] = (low[0] & 0x1f) | (temperature >> 3 & 7) << 5
            out[nb + 16] = (low[1] & 0x1f) | (temperature & 7) << 5
        else:                                   # the original's way
            out[nb + 15:nb + 17] = record[27:29]
        out[nb + 20:nb + 25] = record[32:37]    # experience, training
        out[nb + 39] = record[51]               # walk, swim, fly, ethereal
        male = 0 if record[52] & 2 else 2       # Exult's bit is set for a woman
        out[nb + 40] = (base[52] & ~3) | (record[52] & 1) | male
        out[nb + 81] = record[93]               # food
        out[nb + 89:nb + 105] = record[101:117]     # name
        tree = bytearray()
        if inventory and used:
            # readied things first, so that loading puts them back in their slots
            for obj in sorted(inventory, key=lambda o: o['spot'] < 0):
                self.write_object(obj, record[6], True, tree)
            tree.append(0)
        return bytes(out) + bytes(tree)

    def convert_npcs(self, buf):
        count, extra = struct.unpack_from('<HH', buf, 0)
        npcs, _ = self.parse_npcs(buf, 4, count + extra)
        self.avatar = npcs[0][0]
        base_buf = self.initgame['npc.dat']
        base_count, base_extra = struct.unpack_from('<HH', base_buf, 0)
        bases = []
        p = 4
        for _ in range(base_count + base_extra):
            bases.append(base_buf[p:p + NPC_RECORD])
            ref = struct.unpack_from('<H', base_buf, p + 14)[0]
            contents = struct.unpack_from('<H', base_buf, p + 4)[0]
            shape = struct.unpack_from('<H', base_buf, p + 2)[0] & 0x3ff
            p += NPC_RECORD
            if ref and shape and contents:
                _, p = self.parse_list(base_buf, p, True)
        if count != base_count or extra > base_extra:
            warn(f'Exult has {count}+{extra} NPCs, the game {base_count}+{base_extra}; '
                 'the extra ones are left out')
        out = bytearray(struct.pack('<HH', base_count, base_extra))
        for number in range(base_count + base_extra):
            # the extra NPCs (made by usecode) follow each game's own count
            if number < base_count:
                exult_number = number if number < count else None
            else:
                k = number - base_count
                exult_number = count + k if k < extra else None
            base = bytearray(bases[number])
            if exult_number is not None:
                record, inventory = npcs[exult_number]
                out += self.convert_npc(number, record, inventory, base)
            else:                               # not in Exult's game: unused
                base[4:6] = b'\0\0'
                base[14:16] = b'\0\0'
                out += base
        return bytes(out)

    # the rest

    def convert_flags(self, buf):
        size = len(self.initgame['flaginit'])
        if any(buf[size:]):
            warn(f'Exult set flags past the {size} the game has (a mod?); left out')
        return bytes(1 if b else 0 for b in buf[:size].ljust(size, b'\0'))

    def import_data(self, files):
        """IMPORT.DAT, which the game reads once, as little-endian words: its version, flags
        (1: the avatar can't move, 2: Armageddon is done, 4: infravision), the clock's ticks and
        days, each usecode timer's ticks and days, for Serpent Isle the keyring's 256 bits and
        the position usecode marked, then the party's size and its NPC numbers."""
        gamewin = files['gamewin.dat']
        _, _, day, hour, minute = struct.unpack_from('<5H', gamewin, 0)
        avatar_flags = struct.unpack_from('<I', self.avatar, 68)[0]
        flags = 0
        if avatar_flags & (1 << 16 | 1 << 22):  # Exult's dont_move and bg_dont_move
            flags |= 1
        if len(gamewin) > 22 and gamewin[22] == 1:
            flags |= 2
        if len(gamewin) > 25 and gamewin[25] == 1:
            flags |= 4
        words = [1, flags, hour * 1500 + minute * 25, day]
        use = files.get('usecode.dat', b'')
        party, timers, marked = [], {}, None
        if len(use) >= 18:
            size = struct.unpack_from('<H', use, 0)[0]
            party = list(struct.unpack_from('<8H', use, 2))[:min(size, 8)]
            p = 18
            if struct.unpack_from('<i', use, p)[0] == -1:     # id and hour pairs
                p += 4
                while p + 2 <= len(use):
                    tid = struct.unpack_from('<H', use, p)[0]
                    if tid == 0xffff:
                        break
                    timers[tid] = struct.unpack_from('<I', use, p + 2)[0]
                    p += 6
                p += 2
            else:                               # Exult 1.2: twenty hours in a row
                for tid in range(20):
                    timers[tid] = struct.unpack_from('<I', use, p + 4 * tid)[0]
                p += 80
            if p + 6 <= len(use):                   # where usecode marked the avatar
                marked = struct.unpack_from('<3h', use, p)
        for tid in range(self.timer_count):
            hours = timers.get(tid, 0)
            if hours > 0:
                words += [hours % 24 * 1500, hours // 24]
            else:
                words += [UNSET_TIMER, 0]
        if any(hours for tid, hours in timers.items() if tid >= self.timer_count):
            warn(f'usecode timers past {self.timer_count - 1} are left out')
        if self.serpent_isle:
            keys = bytearray(32)
            ring = files.get('keyring.dat', b'')
            for i in range(0, len(ring) - 1, 2):
                key = struct.unpack_from('<H', ring, i)[0]
                if key < 256:
                    keys[key // 8] |= 1 << key % 8
                else:
                    warn(f'key {key} on the keyring is past the 255 the game knows')
            words += list(struct.unpack('<16H', keys))
            if marked is None or not 0 <= marked[2] <= 15 or marked[0] < 0 or marked[1] < 0:
                marked = (0, 0, 0)              # unset, as in a new game
            words += list(marked)
        party = [n for n in party if n]
        words += [len(party)] + party
        return struct.pack(f'<{len(words)}H', *words)

    def convert(self, files):
        if 'npc.dat' not in files or 'gamewin.dat' not in files:
            sys.exit('exult2u7: not an Exult saved game (no npc.dat or gamewin.dat)')
        if files.get('identity', self.initgame['identity']) != self.initgame['identity']:
            sys.exit('exult2u7: the saved game is of another game than the STATIC folder')
        out = {'NPC.DAT': self.convert_npcs(files['npc.dat']),
               'FLAGINIT': self.convert_flags(files.get('flaginit', b'')),
               'FRAMES.FLG': self.initgame['frames.flg'],
               'IDENTITY': self.initgame['identity'],
               'IMPORT.DAT': self.import_data(files)}
        for region in range(144):
            name = f'u7ireg{region:02x}'
            buf = files.get(name, self.initgame.get(name))
            if buf is not None:
                out[name.upper()] = self.convert_region(buf, region)
        monsters = files.get('monsnpcs.dat', b'')
        if len(monsters) >= 2 and struct.unpack_from('<H', monsters, 0)[0]:
            count = struct.unpack_from('<H', monsters, 0)[0]
            warn(f'{count} monsters roaming the map are left out')
        gamewin = files['gamewin.dat']
        if len(gamewin) >= 14:
            day, hour, minute, light = struct.unpack_from('<3HI', gamewin, 4)
            if light > (day * 24 + hour) * 60 + minute:
                warn('the Light spell still shining is left out')
        return out


def write_save(files, title, path):
    """A saved game: a Flex of 300 slots, each file a 13-byte name and its data."""
    slots = 300
    header = bytearray(128)
    header[0:80] = title.encode('latin-1')[:80].ljust(80, b'\0')
    header[81] = 0x1a
    struct.pack_into('<hiiiiii', header, 82, -1, slots, 0xcc, SAVE_VERSION, 0, 13, -1)
    header[108] = 1
    table = bytearray(8 * slots)
    body = bytearray()
    offset = len(header) + len(table)
    for i, (name, data) in enumerate(sorted(files.items())):
        entry = name.encode('ascii').ljust(13, b'\0') + data
        struct.pack_into('<ii', table, 8 * i, offset + len(body), len(entry))
        body += entry
    with open(path, 'wb') as f:
        f.write(header + table + body)


def main():
    args = sys.argv[1:]
    title = None
    if len(args) >= 2 and args[0] == '--title':
        title = args[1]
        args = args[2:]
    if len(args) != 3:
        sys.exit(__doc__.strip())
    source, static, target = args
    exult_title, files = read_exult_save(source)
    if title is None:
        title = f'{exult_title or "Exult game"} (Exult)'
    converted = Converter(static).convert(files)
    if os.path.isdir(target):
        for name, data in converted.items():
            with open(os.path.join(target, name), 'wb') as f:
                f.write(data)
    else:
        write_save(converted, title, target)
    for text in warnings:
        print('exult2u7:', text, file=sys.stderr)
    print(f'exult2u7: wrote {target}, "{title}"')


if __name__ == '__main__':
    main()
