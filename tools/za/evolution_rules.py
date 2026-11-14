"""Bounds-checked, append-only offline evolution edits for privately dumped ZA data.

Schema facts were checked against KM-Editor's ZaPersonalTables.cs (af23fcd4).
This module contains no game data. New vectors are appended so unknown original
structures need no rebuild.
"""
from __future__ import annotations

from dataclasses import dataclass
import struct


class EvolutionRuleError(ValueError):
    """Unsupported or malformed input; callers must not publish partial output."""


_TRADE = {5, 6, 7}
_EVOLUTION = struct.Struct('<8H')


class _FB:
    def __init__(self, data: bytes):
        if not isinstance(data, bytes):
            raise EvolutionRuleError('FlatBuffer input must be bytes')
        self.data = data
        self.span(0, 4)

    def span(self, pos: int, size: int) -> int:
        if pos < 0 or size < 0 or pos > len(self.data) - size:
            raise EvolutionRuleError('FlatBuffer range is out of bounds')
        return pos

    def uint(self, pos: int, width: int = 4) -> int:
        self.span(pos, width)
        return int.from_bytes(self.data[pos:pos + width], 'little')

    def int32(self, pos: int) -> int:
        self.span(pos, 4)
        return struct.unpack_from('<i', self.data, pos)[0]

    def indirect(self, pos: int) -> int:
        relative = self.uint(pos)
        if relative < 4:
            raise EvolutionRuleError('Invalid forward FlatBuffer offset')
        return self.span(pos + relative, 4)

    def table(self, pos: int) -> '_Table':
        self.span(pos, 4)
        distance = self.int32(pos)
        if distance == 0:
            raise EvolutionRuleError('Missing FlatBuffer vtable')
        vtable = self.span(pos - distance, 4)
        length, size = struct.unpack_from('<HH', self.data, vtable)
        if length < 4 or length % 2 or size < 4:
            raise EvolutionRuleError('Invalid FlatBuffer table size')
        self.span(vtable, length)
        self.span(pos, size)
        offsets = tuple(self.uint(vtable + i, 2) for i in range(4, length, 2))
        if any(offset and not 4 <= offset < size for offset in offsets):
            raise EvolutionRuleError('Invalid FlatBuffer field offset')
        return _Table(self, pos, size, offsets)

    def root(self) -> '_Table':
        return self.table(self.indirect(0))

    def vector(self, reference: int, stride: int) -> tuple[int, int]:
        start = self.indirect(reference)
        count = self.uint(start)
        self.span(start + 4, count * stride)
        return start + 4, count


@dataclass(frozen=True)
class _Table:
    fb: _FB
    pos: int
    size: int
    offsets: tuple[int, ...]

    def field(self, index: int, width: int) -> int | None:
        offset = self.offsets[index] if index < len(self.offsets) else 0
        if not offset:
            return None
        if offset + width > self.size:
            raise EvolutionRuleError('FlatBuffer field exceeds table object')
        return self.fb.span(self.pos + offset, width)

    def scalar(self, index: int, width: int = 4, signed: bool = False) -> int:
        pos = self.field(index, width)
        if pos is None:
            return 0
        return self.fb.int32(pos) if signed else self.fb.uint(pos, width)


def _tables(fb: _FB) -> list[tuple[int, _Table]]:
    reference = fb.root().field(0, 4)
    if reference is None:
        raise EvolutionRuleError('Missing root entry vector')
    start, count = fb.vector(reference, 4)
    return [(pos, fb.table(fb.indirect(pos))) for pos in range(start, start + count * 4, 4)]


@dataclass(frozen=True)
class _Personal:
    index: int
    species: int
    form: int
    present: bool
    reference: int | None
    rules: tuple[tuple[int, ...], ...]


def _personal_rows(data: bytes) -> list[_Personal]:
    fb = _FB(data)
    rows = []
    for index, (_, table) in enumerate(_tables(fb)):
        present = table.scalar(1, 1)
        if present not in (0, 1):
            raise EvolutionRuleError('Invalid personal presence flag')
        identity = table.field(0, 20)
        species, form = struct.unpack_from('<HH', data, identity) if identity is not None else (0, 0)
        if present and not species:
            raise EvolutionRuleError('Present personal row has no species identity')
        reference = table.field(21, 4)
        if reference is None:
            rules = ()
        else:
            start, count = fb.vector(reference, _EVOLUTION.size)
            rules = tuple(_EVOLUTION.unpack_from(data, start + i * _EVOLUTION.size) for i in range(count))
        rows.append(_Personal(index, species, form, bool(present), reference, rules))
    return rows


def _minimum(value: int) -> int:
    if isinstance(value, bool) or not isinstance(value, int) or not 36 <= value <= 100:
        raise EvolutionRuleError('Minimum level must be an integer from 36 to 100')
    return value


def _align(out: bytearray, alignment: int = 4) -> None:
    out.extend(b'\0' * (-len(out) % alignment))


def _point(out: bytearray, reference: int, target: int) -> None:
    relative = target - reference
    if not 4 <= relative <= 0xffffffff:
        raise EvolutionRuleError('Appended resource exceeds FlatBuffer offset range')
    struct.pack_into('<I', out, reference, relative)


def transform_personal(data: bytes, minimum_level: int = 36) -> tuple[bytes, dict]:
    """Replace present trade evolutions with level-up rules without required items.

    Kantonian Slowpoke retains its normal Slowbro level threshold during the day;
    Slowking becomes the night branch. No other non-trade rule is changed. This
    edits rule data; it neither changes a save nor simulates an evolution event.
    """
    minimum_level = _minimum(minimum_level)
    rows = _personal_rows(data)
    out = bytearray(data)
    changes = []
    trade_count = held_count = branch_count = 0
    for row in rows:
        if not row.present or not any(rule[1] in _TRADE for rule in row.rules):
            continue
        slowking = [i for i, rule in enumerate(row.rules)
                    if rule[1] in _TRADE and rule[6:] == (199, 0)]
        split_slowpoke = row.species == 79 and row.form == 0 and bool(slowking)
        slowbro = []
        if split_slowpoke:
            slowbro = [i for i, rule in enumerate(row.rules)
                       if rule[1] == 4 and rule[6:] == (80, 0)]
            if len(slowking) != 1 or len(slowbro) != 1:
                raise EvolutionRuleError('Unsupported Slowpoke evolution branches')
            if row.rules[slowbro[0]][0] != 37 or row.rules[slowbro[0]][2] != 0:
                raise EvolutionRuleError('Unexpected Slowbro level-up requirement')
        rules = []
        plain_count = held_row_count = 0
        for index, rule in enumerate(row.rules):
            level, method, argument, r3, r4, r5, species, form = rule
            if method in _TRADE:
                if not species:
                    raise EvolutionRuleError('Trade rule has no target species')
                method_out = 33 if split_slowpoke and index == slowking[0] else 4
                rules.append((minimum_level, method_out, 0, r3, r4, r5, species, form))
                held_row_count += method == 6
                plain_count += method != 6
            elif split_slowpoke and index == slowbro[0]:
                rules.append((level, 32, 0, r3, r4, r5, species, form))
            else:
                rules.append(rule)
        _align(out)
        vector = len(out)
        out.extend(struct.pack('<I', len(rules)))
        out.extend(b''.join(_EVOLUTION.pack(*rule) for rule in rules))
        _point(out, row.reference, vector)
        trade_count += plain_count + held_row_count
        held_count += held_row_count
        branch_count += split_slowpoke
        changes.append({'row_index': row.index, 'species': row.species, 'form': row.form,
                        'before_rules': len(row.rules), 'after_rules': len(rules),
                        'plain_trade_rules': plain_count, 'held_trade_rules': held_row_count,
                        'slowpoke_day_night_split': split_slowpoke})
    result = bytes(out)
    _personal_rows(result)  # Check every appended vector before returning output.
    return result, {'minimum_level': minimum_level, 'rows_scanned': len(rows),
                    'present_rows': sum(row.present for row in rows),
                    'changed_rows': len(changes), 'trade_rules_changed': trade_count,
                    'held_item_rules_changed': held_count, 'item_requirements_removed': held_count,
                    'slowpoke_branch_rows_changed': branch_count, 'output_rules_added': 0,
                    'changes': changes}
