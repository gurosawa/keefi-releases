import copy
import unittest

import compare
import recalc
from legacy_formats import CURRENT_BUNDLE, CURRENT_RESULT, LEGACY_BUNDLE, LEGACY_RESULT, normalize_bundle
from test_compare import bundle


class LegacyExports(unittest.TestCase):
    def test_previous_export_preserves_values_and_verdict(self):
        current = bundle()
        old = copy.deepcopy(current)
        old['format'] = LEGACY_BUNDLE
        old[LEGACY_RESULT] = old.pop(CURRENT_RESULT)
        before = copy.deepcopy(old)
        self.assertEqual(compare.compare_bundle(old), compare.compare_bundle(current))
        self.assertEqual(recalc.evaluate(recalc.Bundle.load(old)), recalc.evaluate(recalc.Bundle.load(current)))
        self.assertEqual(old, before)

    def test_alias_does_not_hide_numeric_mismatches(self):
        old = bundle()
        old['format'] = LEGACY_BUNDLE
        old[LEGACY_RESULT] = old.pop(CURRENT_RESULT)
        old[LEGACY_RESULT]['rows'][0]['value'] = '999999'
        self.assertFalse(compare.compare_bundle(old)['matched'])

    def test_mixed_envelopes_are_rejected(self):
        for format_name in (LEGACY_BUNDLE, CURRENT_BUNDLE):
            mixed = {'format': format_name, LEGACY_RESULT: {}, CURRENT_RESULT: {}}
            with self.assertRaises(ValueError):
                normalize_bundle(mixed)


if __name__ == '__main__':
    unittest.main()
