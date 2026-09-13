"""Native exported-tokenizer parity with the previous Transformers path."""

from __future__ import annotations

import builtins
import json
import tempfile
import unittest
from pathlib import Path
from unittest import mock

from recall_gateway import RERANK_MAX_LEN, OnnxReranker


class NativeTokenizerTests(unittest.TestCase):
    def test_exported_pairs_match_transformers_without_framework_imports(self):
        try:
            import numpy as np
            from tokenizers import BertWordPieceTokenizer
            from transformers import BertTokenizerFast
        except ImportError as exc:
            self.skipTest(f"optional model dependencies unavailable: {exc}")
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            (root / "vocab.txt").write_text(
                "[PAD]\n[UNK]\n[CLS]\n[SEP]\n[MASK]\nhello\nworld\n?\ncafe\nsm\n##riti\n"
            )
            tokenizer = BertWordPieceTokenizer(str(root / "vocab.txt"), lowercase=True)
            tokenizer.save(str(root / "tokenizer.json"))
            (root / "tokenizer_config.json").write_text(
                json.dumps({"tokenizer_class": "BertTokenizerFast", "pad_token": "[PAD]"})
            )
            previous = BertTokenizerFast.from_pretrained(directory, local_files_only=True)
            session = mock.Mock()
            session.get_inputs.return_value = [mock.Mock(name="input") for _ in range(3)]
            for item, name in zip(
                session.get_inputs.return_value, ("input_ids", "attention_mask", "token_type_ids")
            ):
                item.name = name
            session.run.return_value = [np.zeros((5, 1))]
            ort = mock.Mock()
            ort.InferenceSession.return_value = session
            original_import = builtins.__import__

            def no_framework(name, *args, **kwargs):
                if name.split(".")[0] in ("torch", "transformers"):
                    raise AssertionError("Native ONNX loader imported " + name)
                return original_import(name, *args, **kwargs)

            with (
                mock.patch.dict("sys.modules", onnxruntime=ort),
                mock.patch("builtins.__import__", side_effect=no_framework),
            ):
                reranker = OnnxReranker(directory)
            pairs = [
                ("hello", "world"),
                ("SMRITI?", "Café"),
                ("", ""),
                ("hello " * 200, "world " * 300),
                ("hello", "world " * 400),
            ]
            reranker.predict(pairs)
            actual = session.run.call_args.args[1]
            expected = previous(
                [p[0] for p in pairs],
                [p[1] for p in pairs],
                padding=True,
                truncation=True,
                max_length=RERANK_MAX_LEN,
                return_tensors="np",
            )
            for name in actual:
                np.testing.assert_array_equal(actual[name], expected[name], err_msg=name)
            session.run.reset_mock()
            self.assertEqual(reranker.predict([]), [])
            session.run.assert_not_called()
