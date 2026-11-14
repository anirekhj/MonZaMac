#!/usr/bin/env python3
"""Authored FlatBuffer fixtures only; no game, decoder, or controller required."""
from __future__ import annotations

from pathlib import Path
import struct
import sys
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parent / 'za'))
from evolution_rules import EvolutionRuleError, transform_personal


def align(out, n=4):
    out.extend(b'\0' * (-len(out) % n))


def table(out, fields, last=None):
    """Small forward-writing fixture builder, independent of production parser."""
    last = max(fields, default=-1) if last is None else last
    offsets, body = {}, bytearray(4)
    for index, data in sorted(fields.items()):
        align(body)
        offsets[index] = len(body)
        body.extend(data)
    align(body)
    align(out, 2)
    vtable = len(out)
    out.extend(struct.pack('<HH', 4 + 2 * (last + 1), len(body)))
    for index in range(last + 1):
        out.extend(struct.pack('<H', offsets.get(index, 0)))
    align(out)
    pos = len(out)
    struct.pack_into('<i', body, 0, pos - vtable)
    out.extend(body)
    return pos, {index: pos + offset for index, offset in offsets.items()}


def pointer(out, reference, target):
    struct.pack_into('<I', out, reference, target - reference)


def vector(out, entries):
    align(out)
    pos = len(out)
    out.extend(struct.pack('<I', len(entries)))
    for entry in entries:
        out.extend(entry)
    return pos


def fixture(rows):
    out = bytearray(4)
    root, fields = table(out, {0: b'\0' * 4})
    pointer(out, 0, root)
    entries = vector(out, [b'\0' * 4 for _ in rows])
    pointer(out, fields[0], entries)
    metadata = []
    for index, row in enumerate(rows):
        species, form, present, rules = row
        identity = struct.pack('<3H2B2H3BxI', species, form, 123, 4, 5, 90, 80, 7, 8, 9, 456)
        pos, fields = table(out, {0: identity, 1: bytes([present]),
                                  21: b'\0' * 4, 30: b'\0' * 4}, last=30)
        pointer(out, entries + 4 + index * 4, pos)
        evolution_vector = vector(out, [struct.pack('<8H', *rule) for rule in rules])
        pointer(out, fields[21], evolution_vector)
        unknown = vector(out, [b'authored-unknown-payload'])
        pointer(out, fields[30], unknown)
        metadata.append({'reference': fields[21], 'vector': evolution_vector,
                         'table': pos, 'present': fields[1], 'unknown': fields[30]})
    return bytes(out), metadata


def rules_at(data, reference):
    start = reference + struct.unpack_from('<I', data, reference)[0]
    count = struct.unpack_from('<I', data, start)[0]
    return [struct.unpack_from('<8H', data, start + 4 + i * 16) for i in range(count)]


def rule(method, argument=0, target=901, form=0, level=0):
    return (level, method, argument, 11, 22, 33, target, form)


def assert_preserved(test, original, output, references):
    allowed = {byte for ref in references for byte in range(ref, ref + 4)}
    for i, (before, after) in enumerate(zip(original, output)):
        if i not in allowed:
            test.assertEqual(before, after, f'Unexpected original-byte change at {i}')


class EvolutionTests(unittest.TestCase):
    def test_plain_trade_level_and_preserved_fields(self):
        source, meta = fixture([(100, 3, 1, [rule(5, 99, form=4), rule(7, 502)])])
        output, report = transform_personal(source)
        actual = rules_at(output, meta[0]['reference'])
        self.assertEqual(actual, [rule(4, form=4, level=36), rule(4, level=36)])
        self.assertEqual(report['trade_rules_changed'], 2)
        self.assertEqual(report['changed_rows'], 1)
        self.assertEqual(report['changes'][0]['form'], 3)
        assert_preserved(self, source, output, [meta[0]['reference']])

    def test_minimum_validation_and_exact_configured_level(self):
        source, meta = fixture([(100, 0, 1, [rule(5, level=40)])])
        for invalid in (35, 0, -1, 101, True, 36.0, '36', None):
            with self.subTest(invalid=invalid), self.assertRaises(EvolutionRuleError):
                transform_personal(source, invalid)
        for minimum in (36, 37, 100):
            output, _ = transform_personal(source, minimum)
            self.assertEqual(rules_at(output, meta[0]['reference'])[0][0], minimum)

    def test_held_item_removed_without_mapping_or_time_requirement(self):
        nontrade = [rule(4, target=902, level=37), rule(32, target=903, level=45)]
        source, meta = fixture([(100, 0, 1, [nontrade[0], rule(6, 888, form=7), nontrade[1]])])
        output, report = transform_personal(source)
        self.assertEqual(rules_at(output, meta[0]['reference']),
                         [nontrade[0], rule(4, form=7, level=36), nontrade[1]])
        self.assertEqual(report['held_item_rules_changed'], 1)
        self.assertEqual(report['item_requirements_removed'], 1)
        self.assertEqual(report['output_rules_added'], 0)
        assert_preserved(self, source, output, [meta[0]['reference']])

    def test_absent_sources_and_unrelated_rules_unchanged(self):
        source, meta = fixture([(100, 0, 0, [rule(6, 888)]),
                                (101, 0, 1, [rule(8, 42), rule(28, level=30)])])
        output, report = transform_personal(source)
        self.assertEqual(output, source)
        self.assertEqual(report['changed_rows'], 0)
        self.assertEqual(report['present_rows'], 1)

    def test_all_source_and_target_forms_preserved(self):
        source, meta = fixture([(100, form, 1, [rule(6, 888, form=form)]) for form in range(4)])
        output, report = transform_personal(source)
        self.assertEqual(report['changed_rows'], 4)
        for form, entry in enumerate(meta):
            self.assertEqual(rules_at(output, entry['reference']), [rule(4, form=form, level=36)])
        self.assertEqual([r['form'] for r in report['changes']], list(range(4)))

    def test_slowpoke_day_night_branches_preserve_levels_and_order(self):
        slowbro = rule(4, target=80, level=37)
        slowking = rule(6, 888, target=199)
        unrelated = rule(8, 123, target=900)
        source, meta = fixture([(79, 0, 1, [slowbro, unrelated, slowking]),
                                (79, 1, 1, [rule(8, 123, target=80, form=1)]),
                                (100, 0, 1, [rule(6, 888, target=199)])])
        output, report = transform_personal(source)
        self.assertEqual(rules_at(output, meta[0]['reference']),
                         [rule(32, target=80, level=37), unrelated, rule(33, target=199, level=36)])
        self.assertEqual(rules_at(output, meta[1]['reference']), [rule(8, 123, target=80, form=1)])
        self.assertEqual(rules_at(output, meta[2]['reference']), [rule(4, target=199, level=36)])
        self.assertEqual(report['slowpoke_branch_rows_changed'], 1)
        assert_preserved(self, source, output, [meta[0]['reference'], meta[2]['reference']])
        output, _ = transform_personal(source, minimum_level=40)
        self.assertEqual(rules_at(output, meta[0]['reference'])[0][0], 37)
        self.assertEqual(rules_at(output, meta[0]['reference'])[2][0], 40)

    def test_slowpoke_unknown_or_ambiguous_layout_fails_closed(self):
        slowbro = rule(4, target=80, level=37)
        slowking = rule(6, 888, target=199)
        for rules in ([slowking], [slowbro, slowking, slowking],
                      [slowbro, slowbro, slowking], [rule(4, target=80, level=38), slowking],
                      [rule(4, 7, target=80, level=37), slowking]):
            source, _ = fixture([(79, 0, 1, rules)])
            with self.subTest(rules=rules), self.assertRaises(EvolutionRuleError):
                transform_personal(source)

    def test_rerun_is_byte_identical(self):
        source, _ = fixture([(100, 0, 1, [rule(5), rule(6, 888)])])
        first, _ = transform_personal(source)
        second, report = transform_personal(first)
        self.assertEqual(second, first)
        self.assertEqual(report['changed_rows'], 0)

    def test_malformed_personal_ranges_and_values(self):
        source, meta = fixture([(100, 0, 1, [rule(5)])])
        cases = [b'', source[:3], source[:10], source[:meta[0]['vector'] + 8]]
        for offset, value in [(0, 0xffffffff), (meta[0]['reference'], 0xffffffff),
                              (meta[0]['vector'], 0xffffffff), (meta[0]['table'], 0),
                              (meta[0]['table'], 0x7fffffff)]:
            bad = bytearray(source)
            struct.pack_into('<I', bad, offset, value)
            cases.append(bytes(bad))
        bad = bytearray(source)
        bad[meta[0]['present']] = 2
        cases.append(bytes(bad))
        for index, bad in enumerate(cases):
            with self.subTest(case=index), self.assertRaises(EvolutionRuleError):
                transform_personal(bad)
        for bad_rule in (rule(5, target=0),):
            bad, _ = fixture([(100, 0, 1, [bad_rule])])
            with self.assertRaises(EvolutionRuleError):
                transform_personal(bad)

    def test_shared_original_vector_only_repoints_present_owner(self):
        source, meta = fixture([(100, 0, 1, [rule(5)]), (101, 0, 0, [rule(5)])])
        shared = bytearray(source)
        # Use a forward shared vector appended after both table owners.
        shared_vector = vector(shared, [struct.pack('<8H', *rule(5))])
        for entry in meta:
            pointer(shared, entry['reference'], shared_vector)
        shared = bytes(shared)
        output, _ = transform_personal(shared)
        self.assertEqual(rules_at(output, meta[1]['reference']), [rule(5)])
        self.assertEqual(rules_at(output, meta[0]['reference']), [rule(4, level=36)])
        assert_preserved(self, shared, output, [meta[0]['reference']])

    def test_signed_shared_vtable_reference(self):
        source, meta = fixture([(100, 0, 1, [rule(5)])])
        out = bytearray(source)
        pos = meta[0]['table']
        old_vtable = pos - struct.unpack_from('<i', out, pos)[0]
        size = struct.unpack_from('<H', out, old_vtable)[0]
        align(out, 2)
        new_vtable = len(out)
        out.extend(out[old_vtable:old_vtable + size])
        struct.pack_into('<i', out, pos, pos - new_vtable)
        transformed, _ = transform_personal(bytes(out))
        self.assertEqual(rules_at(transformed, meta[0]['reference']), [rule(4, level=36)])


if __name__ == '__main__':
    unittest.main()
