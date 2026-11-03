#!/usr/bin/env python3
"""Exercise missing metadata and a truthful complete trailer block."""
import importlib.util, pathlib, unittest
path=pathlib.Path(__file__).resolve().parents[1]/'scripts/check-commit-metadata.py'
spec=importlib.util.spec_from_file_location('metadata',path);m=importlib.util.module_from_spec(spec);spec.loader.exec_module(m)
class Metadata(unittest.TestCase):
    def test_missing(self):self.assertTrue(m.validate('No provenance\n'))
    def test_valid_and_identity(self):
        msg='Test\n\nAgent-Name: Test agent\nModel-Name: Test model\nModel-Variant: Test variant\nModel-ID: test-only\nReasoning-Effort: high\nModel-Version: not-exposed\n'
        self.assertEqual([],m.validate(msg,'Test agent'))
        self.assertTrue(m.validate(msg,'Human identity'))
if __name__=='__main__':unittest.main()
