import importlib.util
from pathlib import Path
import unittest

SCRIPT = Path(__file__).resolve().parents[2] / 'scripts/tools/update-steam-graphics-index.py'
spec = importlib.util.spec_from_file_location('steam_graphics_index_gen', SCRIPT)
gen = importlib.util.module_from_spec(spec)
spec.loader.exec_module(gen)


def page(api, appid='123', date='2020-01-01', page_id=1):
    return gen.parse_page(page_id, 'Game', '{{Infobox game|steam appid=' + appid + '|release dates={{Infobox game/row/date|Windows|' + date + '}}}}\n{{API|' + api + '}}')


class ParserTests(unittest.TestCase):
    def test_graphics_tiers(self):
        for api, backend in [('12', 'neptune'), ('DX11', 'neptune'), ('10', 'mullion'), ('D3D10.1', 'mullion'), ('9.0c', 'venus'), ('true', 'venus'), ('unknown', 'venus')]:
            with self.subTest(api=api):
                self.assertEqual(page('direct3d versions=' + api)['backend'], backend)
        self.assertEqual(page('vulkan versions=1.3')['backend'], 'venus')
        self.assertTrue(page('vulkan versions=1.3')['hasVulkan'])
        self.assertTrue(page('opengl versions=4')['hasOpenGL'])

    def test_appid_exact_and_annotation_masking(self):
        text = '''{{Infobox game|steam appid side=55|steam appid=730, 001 0 4294967296 {{note|900}} 42}}
<!-- {{API|direct3d versions=12}} -->
<ref name="x">{{API|direct3d versions=12}}</ref><nowiki>{{API|direct3d versions=12}}</nowiki>
{{note|{{API|direct3d versions=12}}}}{{API|direct3d versions=9.0c}}'''
        candidate = gen.parse_page(1, 'x', text)
        self.assertEqual(candidate['appIDs'], [42, 730])
        self.assertEqual(candidate['backend'], 'venus')
        self.assertEqual(gen.parse_page(1, '', '{{Infobox game|steam appid side=55}}')['appIDs'], [])

    def test_dates_and_disambiguation(self):
        old = page('direct3d versions=9', '730', '2012-08-21', 1)
        new = page('direct3d versions=11', '730', '2023-09-27', 2)
        self.assertGreater(gen.candidate_key(new), gen.candidate_key(old))
        self.assertGreater(gen.candidate_key(old), gen.candidate_key(page('direct3d versions=12', '730', 'unknown', 3)))
        self.assertLess(gen.candidate_key(page('direct3d versions=10', '730', '2020', 1)), gen.candidate_key(page('direct3d versions=12', '730', '2020', 2)))
        self.assertGreater(gen.candidate_key(page('direct3d versions=12', '730', '2020', 1)), gen.candidate_key(page('direct3d versions=12', '730', '2020', 2)))
        text = '{{Infobox game|release dates={{Infobox game/row/date|Windows|2020-02-30}}{{Infobox game/row/date|Windows|January 1, 2018}}{{Infobox game/row/date|Windows|2019}}}}'
        self.assertEqual(gen.parse_page(1, '', text)['windowsReleaseDate'], (2018, 1, 1))

    def test_output_disjoint_sorted(self):
        candidates = {'5': page('direct3d versions=12', '5'), '2': page('direct3d versions=12', '2'), '3': page('direct3d versions=10', '3'), '1': page('vulkan versions=1', '1')}
        output = gen.make_output(candidates)
        self.assertEqual(output['schemaVersion'], 1)
        self.assertEqual(output['source'], 'PCGamingWiki')
        self.assertTrue(output['generatedAt'])
        self.assertEqual(output['neptune'], [2, 5])
        self.assertEqual(output['mullion'], [3])
        self.assertEqual(output['venus'], [1])
        all_ids = sum((output[k] for k in ('neptune', 'mullion', 'venus')), [])
        self.assertEqual(len(all_ids), len(set(all_ids)))


if __name__ == '__main__':
    unittest.main()
