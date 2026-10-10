#!/usr/bin/env python3
"""Pokétch cache-miss fallback and unified LCD composition regressions."""
import unittest
import gen_manifest as gen


class PoketchManifestTests(unittest.TestCase):
    def test_pending_image_has_same_geometry_fallback_beneath_it(self):
        for full, page in [(False, gen.page_poketch()), (True, gen.page_poketch_full())]:
            prefix = 'lp.pktx.' if full else 'lp.pkt.'
            widgets = page['widgets']
            for suffix in ['', 'c']:
                fallback = next(i for i, w in enumerate(widgets)
                                if w.get('src_bind') == prefix + 'fallback' + suffix)
                image = next(i for i, w in enumerate(widgets)
                             if w.get('src_bind') == prefix + 'image' + suffix)
                self.assertLess(fallback, image)
                self.assertEqual(widgets[fallback]['rect'], widgets[image]['rect'])
                for field in ['hide_bind', 'hide_eq', 'need_bind']:
                    self.assertEqual(widgets[fallback].get(field), widgets[image].get(field))

    def test_app_content_cannot_bypass_lcd_texture_and_scaling(self):
        for full in [False, True]:
            widgets, _ = gen.lcd_layer(0, 0, gen.FULL_S if full else 1.0, full)
            pictures = [w for w in widgets if w['type'] == 'image']
            self.assertEqual(len(pictures), 4)  # two themes: fallback and opaque LCD
            for widget in pictures:
                self.assertNotIn('rotate_bind', widget)
                self.assertNotIn('scale_bind', widget)
            self.assertFalse(any(w['type'] == 'label' for w in widgets))


if __name__ == '__main__':
    unittest.main()
