"""Exercise JSON transport with entities that broke CLI replay."""
import json
import socket
import tempfile
import threading
import unittest
from pathlib import Path

from evaluate_replay import connect_relation


class ReplayTransportTest(unittest.TestCase):
    def test_flag_entities_are_data_and_errors_propagate(self):
        with tempfile.TemporaryDirectory(prefix="p13-wire-", dir="/projects/caeg/scratch/kbd606/tmp") as root:
            path = str(Path(root) / "rpc.sock")
            seen = []
            with socket.socket(socket.AF_UNIX, socket.SOCK_STREAM) as server:
                server.bind(path)
                server.listen(2)
                def serve():
                    for response in ({"result": {"ok": True}}, {"error": {"message": "rejected"}}):
                        with server.accept()[0] as conn:
                            with conn.makefile("rb") as stream:
                                seen.append(json.loads(stream.readline()))
                            conn.sendall((json.dumps(response) + "\n").encode())
                worker = threading.Thread(target=serve)
                worker.start()
                self.assertEqual(connect_relation(path, "--help", "uses", "--realm"), {"ok": True})
                with self.assertRaisesRegex(ValueError, "rejected"):
                    connect_relation(path, "a", "uses", "b")
                worker.join(timeout=5)
                self.assertFalse(worker.is_alive())
            self.assertEqual(seen[0]["params"]["arguments"],
                             {"subject": "--help", "predicate": "uses", "object": "--realm"})
