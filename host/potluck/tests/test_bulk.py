"""M25: the bulk channel -- the PC's file service (potluck.services), the hub's transfers (potluck.bulk), the cluster
file's bulk.folder, and the file-hash test app's compiled config."""

import json
import struct
import sys
import tempfile
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))

from potluck import bulk as bk  # noqa: E402
from potluck import services as sv  # noqa: E402
from potluck.paths import path_hash  # noqa: E402


class FileService(unittest.TestCase):
    def setUp(self) -> None:
        self.tmp = tempfile.TemporaryDirectory()
        self.root = Path(self.tmp.name) / "offered"
        self.root.mkdir()
        (self.root / "test.bin").write_bytes(bytes(range(256)) * 4)
        (self.root / ".hidden").write_bytes(b"x")
        (Path(self.tmp.name) / "secret.txt").write_bytes(b"not offered")
        self.started = []
        self.ps = sv.PcServices("lab", ["file"], str(self.root), lambda *a: self.started.append(a))

    def tearDown(self) -> None:
        self.tmp.cleanup()

    def ask(self, name: str) -> int:
        args = struct.pack("<BI", 1, 0xC0FFEE) + name.encode()
        v = self.ps.serve("file", 0x8160, args)
        return int.from_bytes(bytes(v.raw[:4]), "little")

    def test_a_file_in_the_folder_is_answered_with_its_size_and_handed_to_the_hub(self) -> None:
        self.assertEqual(self.ask("test.bin"), 1024)
        caller, path, name, data = self.started[0]
        self.assertEqual((caller, path, name, len(data)), (0x8160, 0xC0FFEE, "test.bin", 1024))
        self.assertEqual(self.ps.files(), [{"name": "test.bin", "bytes": 1024}])

    def test_nothing_outside_the_folder_and_no_hidden_or_missing_file(self) -> None:
        for name in ("../secret.txt", "..\\secret.txt", ".hidden", "nope.bin", "", "a/b", "x" * 65):
            self.assertEqual(self.ask(name), sv.NO_FILE, name)
        self.assertEqual(self.started, [])
        self.assertEqual(self.ps.snapshot()[0]["errors"], 7)

    def test_without_a_folder_there_is_no_file(self) -> None:
        ps = sv.PcServices("lab", ["file"])
        v = ps.serve("file", 1, struct.pack("<BI", 1, 1) + b"test.bin")
        self.assertEqual(int.from_bytes(bytes(v.raw[:4]), "little"), sv.NO_FILE)


class Transfers(unittest.TestCase):
    def make(self, size: int, reachable=True):
        sent, now = [], [0.0]
        b = bk.Bulk(lambda board, h, args: reachable and not sent.append((board, h, args)), clock=lambda: now[0])
        b._thread = object()  # no sender thread: the test pumps by hand
        t = b.start(0x8160, 0xAB, "f", bytes(i % 251 for i in range(size)))
        return b, t, sent, now

    def test_a_window_of_chunks_then_more_as_the_app_answers(self) -> None:
        b, t, sent, now = self.make(1000)
        b.pump()
        self.assertEqual(len(sent), bk.WINDOW)
        ver, xfer, off = struct.unpack_from("<BHI", sent[1][2])
        self.assertEqual((ver, xfer, off, len(sent[1][2]) - 7), (1, t.xfer, bk.CHUNK, bk.CHUNK))
        b.ack(t, bk.CHUNK)  # the first chunk taken
        b.pump()
        self.assertEqual(len(sent), bk.WINDOW + 1)
        b.ack(t, 1000)
        self.assertEqual(t.state, "done")
        self.assertEqual(b.by_path(0xAB), None)  # done: answers no longer match it
        self.assertEqual(b.snapshot()[0]["bytes"], 1000)

    def test_quiet_answers_send_again_from_what_the_app_holds(self) -> None:
        b, t, sent, now = self.make(1000)
        b.pump()
        b.ack(t, bk.CHUNK)
        now[0] += bk.RESEND_S + 0.01
        b.pump()
        offs = [struct.unpack_from("<I", a, 3)[0] for _, _, a in sent]
        self.assertEqual(offs[-bk.WINDOW], bk.CHUNK)  # back to what the app holds
        self.assertEqual(t.resent, 1)
        now[0] += bk.GIVE_UP_S
        b.pump()
        self.assertEqual(t.state, "failed")

    def test_an_unreachable_board_keeps_the_chunk_for_the_next_pass(self) -> None:
        b, t, sent, now = self.make(500, reachable=False)
        b.pump()
        self.assertEqual((sent, t.sent), ([], 0))


class HubRouting(unittest.TestCase):
    def test_a_chunks_answer_on_the_cable_moves_the_transfer(self) -> None:
        from potluck import frame as fr
        from potluck import hub as hb
        from potluck.ns_payloads import Reply
        from potluck.transport import LoopbackTransport
        from potluck.value import Value
        host_side, _ = LoopbackTransport.pair()
        hub = hb.Hub(host_side)
        hub.bulk._thread = object()  # no sender thread here
        t = hub.bulk.start(0x8160, 0xAB, "f", b"x" * 500)
        rep = Reply(path_hash=0xAB, timestamp_ms=0, age_ms=0, unit=0, reply_to=fr.Op.CALL, status=0, quality=1,
                    latency_class=4, value=Value.of_u32(384))
        hub._input_reply(fr.parse(fr.encode(src=0x6300, dst=0x00FE, opcode=fr.Op.REPLY, msg_id=0x8001,
                                            payload=rep.encode())))
        self.assertEqual(t.acked, 384)


class Cluster(unittest.TestCase):
    def test_the_folder_is_named_when_file_is_offered(self) -> None:
        from potluck import apply as ap
        with tempfile.TemporaryDirectory() as tmp:
            (Path(tmp) / "files").mkdir()

            def cluster(**kw):
                p = Path(tmp) / "c.json"
                p.write_text(json.dumps({"schema": ap.SCHEMA, "system": "lab", "boards": [{"node": "6300"}],
                                         "firmware": None, **kw}), encoding="utf-8")
                return ap.load_cluster(str(p))
            c = cluster(pc_services=["file"], bulk={"folder": "files"})
            self.assertEqual(Path(c.bulk_folder), Path(tmp).resolve() / "files")
            with self.assertRaisesRegex(ap.ApplyError, "which folder"):
                cluster(pc_services=["file"])
            with self.assertRaisesRegex(ap.ApplyError, "not a folder"):
                cluster(bulk={"folder": "nope"})


class Compile(unittest.TestCase):
    def test_file_hash_compiles_to_the_layout_the_node_parses(self) -> None:
        from potluck import deploy as dp
        from potluck.manifest import parse
        doc = {"schema": 1, "system": "lab", "min_core_version": 1,
               "nodes": [{"node_id": 0x8160, "label": "C"},
                         {"node_id": 0x00FE, "label": "pc", "kind": "host",
                          "owns": [{"path": "potluck://lab/svc/file", "unit": "none", "kind": "sampled",
                                    "access": "read", "latency_class": 4, "staleness_bound_ms": 0}]}],
               "actors": [{"name": "fetch", "module": "builtin:file_hash", "latency_class": 4, "pin": 0x8160,
                           "needs": ["potluck://lab/svc/file"], "config": {"file": "test64k.bin"}}]}
        img = dp.compile_image(parse(doc), 1)
        at = dp.IMAGE_HEADER_LEN
        node, typ, n = struct.unpack_from("<HBB", img, at)
        self.assertEqual((node, typ, n), (0x8160, dp.ACTOR_FILE_HASH, 31 + 11))
        svc, provider, *hashes = struct.unpack_from("<IHIIIIII", img, at + 4)
        self.assertEqual((svc, provider), (path_hash("potluck://lab/svc/file"), 0x00FE))
        p = dp.file_hash_paths("lab", "fetch")
        self.assertEqual(hashes, [path_hash(p[k]) for k in ("in", "go", "bytes", "state", "digest", "ms")])
        self.assertEqual(img[at + 4 + 31:at + 4 + 42], b"test64k.bin")
        for bad in ({"file": "../x"}, {"file": ".x"}, {"file": "x", "more": 1}, {}):
            d = json.loads(json.dumps(doc))
            d["actors"][0]["config"] = bad
            with self.assertRaises(dp.DeployError):
                dp.compile_image(parse(d), 1)


if __name__ == "__main__":
    unittest.main()
