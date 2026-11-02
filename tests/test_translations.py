"""Check catalog integrity and the read-only source-extraction boundary."""

from pathlib import Path
import tempfile
import unittest
from unittest.mock import patch
import xml.etree.ElementTree as ET
import sys

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tools"))
from check_translations import check_sources, messages


class TranslationCatalogTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.addCleanup(self.temporary.cleanup)
        self.catalog = Path(self.temporary.name) / "licasa_sv.ts"

    def write(self, entries, language="sv_SE"):
        root = ET.Element("TS", language=language, version="2.1")
        context = ET.SubElement(root, "context")
        ET.SubElement(context, "name").text = "Example"
        for source, translation, options in entries:
            message = ET.SubElement(context, "message", options.get("message", {}))
            ET.SubElement(message, "source").text = source
            if translation is None:
                continue
            target = ET.SubElement(message, "translation", options.get("translation", {}))
            if isinstance(translation, list):
                for variant in translation:
                    ET.SubElement(target, "numerusform").text = variant
            else:
                target.text = translation
        ET.ElementTree(root).write(self.catalog, encoding="utf-8", xml_declaration=True)

    def test_valid_placeholders_can_be_reordered_but_not_dropped_or_duplicated(self):
        self.write([("%1 then %2", "%2 sedan %1", {})])
        self.assertEqual(messages(self.catalog)[1], 1)
        for translation in ("%1", "%1 %2 %2", "%L1 %2"):
            self.write([("%1 then %2", translation, {})])
            with self.subTest(translation=translation), self.assertRaisesRegex(ValueError, "placeholders"):
                messages(self.catalog)

    def test_missing_or_empty_finished_entries_cannot_claim_coverage(self):
        for translation in (None, "", "  "):
            self.write([("Language", translation, {})])
            with self.subTest(translation=translation), self.assertRaises(ValueError):
                messages(self.catalog)

    def test_unfinished_entries_are_explicit_english_fallback(self):
        self.write([("Language", "", {"translation": {"type": "unfinished"}})])
        entries, finished = messages(self.catalog)
        self.assertEqual(len(entries), 1)
        self.assertEqual(finished, 0)

    def test_duplicates_cannot_hide_an_ambiguous_translation(self):
        self.write([("Open", "Öppna", {}), ("Open", "Öppet", {})])
        with self.assertRaisesRegex(ValueError, "Duplicate"):
            messages(self.catalog)

    def test_swedish_plural_forms_and_placeholders_are_complete(self):
        self.write([("%n image(s)", ["%n bild", "%n bilder"], {"message": {"numerus": "yes"}})])
        self.assertEqual(messages(self.catalog)[1], 1)
        for forms in (["%n bild"], ["%n bild", "bilder"], []):
            self.write([("%n image(s)", forms, {"message": {"numerus": "yes"}})])
            with self.subTest(forms=forms), self.assertRaises(ValueError):
                messages(self.catalog)

    def test_retired_entries_do_not_count_as_active_coverage(self):
        self.write([("Old", "Gammal", {"translation": {"type": "obsolete"}})])
        self.assertEqual(messages(self.catalog), ({}, 0))

    def test_missing_target_language_is_rejected(self):
        self.write([("Language", "Språk", {})], language="")
        with self.assertRaisesRegex(ValueError, "target language"):
            messages(self.catalog)

    def test_source_check_uses_a_temporary_catalog_and_preserves_the_original(self):
        self.write([("Language", "Språk", {})])
        original = self.catalog.read_bytes()

        def extract(command, **kwargs):
            output = Path(command[command.index("-ts") + 1])
            self.assertNotEqual(output, self.catalog)
            self.assertEqual(kwargs["timeout"], 60)
            output.write_bytes(original)

        with patch("check_translations.subprocess.run", side_effect=extract):
            check_sources(self.catalog, Path("lupdate"))
        self.assertEqual(self.catalog.read_bytes(), original)

    def test_a_new_untranslated_source_string_fails_the_source_gate(self):
        self.write([("Language", "Språk", {})])

        def extract(command, **kwargs):
            output = Path(command[command.index("-ts") + 1])
            text = self.catalog.read_text().replace("Language", "New source string")
            output.write_text(text)

        with patch("check_translations.subprocess.run", side_effect=extract), \
                self.assertRaisesRegex(ValueError, "1 missing, 1 obsolete"):
            check_sources(self.catalog, Path("lupdate"))

    def test_shipped_swedish_catalog_is_finished_and_preserves_all_placeholders(self):
        entries, finished = messages(ROOT / "translations/licasa_sv.ts")
        self.assertIn(("FullscreenSettingsContent", "Language", "", ""), entries)
        self.assertIn(("FullscreenSideMenu", "Settings", "", ""), entries)
        self.assertIn(("FormatSupport", "All Files (*)", "", ""), entries)
        self.assertEqual(len(entries), finished)


if __name__ == "__main__":
    unittest.main()
