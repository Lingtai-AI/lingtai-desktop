#!/usr/bin/env python3
"""Offline manual bridge; run with an explicit existing Python interpreter.

Required arguments: --kernel-source (clean pinned checkout), --publisher
(built lingtai_direct_mail_publisher_test), --sandbox (new disposable path).
No Agent is started. Real pickup, inbox serialization, reply, and mailman run;
only the final mail service is replaced with a recording transport.
"""
from __future__ import annotations

import argparse
import hashlib
import inspect
import json
from pathlib import Path
import subprocess
import sys
import threading
from types import SimpleNamespace
from unittest.mock import patch

KERNEL_COMMIT = "9506b210ac57264e849bc1c505b44d480f2761fe"


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--kernel-source", required=True, type=Path)
    parser.add_argument("--publisher", required=True, type=Path)
    parser.add_argument("--sandbox", required=True, type=Path)
    args = parser.parse_args()
    kernel = args.kernel_source.resolve(strict=True)
    def git(*command):
        return subprocess.check_output(["git", "-C", str(kernel), *command], text=True).strip()
    assert git("rev-parse", "HEAD") == KERNEL_COMMIT, "kernel must match documented pin"
    assert not git("status", "--porcelain", "--untracked-files=all", "--", "src"), "kernel src must be clean"
    assert not args.sandbox.exists(), "sandbox must be a new disposable path"
    sys.dont_write_bytecode = True
    sys.path.insert(0, str(kernel / "src"))
    from lingtai.adapters.posix.mail import PosixFilesystemMailAdapter
    from lingtai.tools.email.manager import EmailManager
    import lingtai
    assert Path(inspect.getfile(EmailManager)).resolve().is_relative_to(kernel / "src")
    assert Path(inspect.getfile(PosixFilesystemMailAdapter)).resolve().is_relative_to(kernel / "src")
    subprocess.run([str(args.publisher.resolve(strict=True)), str(args.sandbox.resolve()),
                    "--bridge-fixtures"], check=True, timeout=30)
    cases = []
    for name in ("project-a", "project-b"):
        project = args.sandbox.resolve() / name
        human = project / ".lingtai" / "human"
        responder = project / ".lingtai" / "telegram-bot"
        leaf, = (human / "mailbox" / "outbox").iterdir()
        raw_bytes = (leaf / "message.json").read_bytes()
        original = json.loads(raw_bytes)
        eid = leaf.name
        adapter = PosixFilesystemMailAdapter(working_dir=responder)
        received = []
        # Real one-shot production pickup, without starting a polling service.
        adapter._poll_pseudo_outbox(human, received.append)
        inbox = responder / "mailbox" / "inbox" / eid / "message.json"
        picked = json.loads(inbox.read_text())
        assert received == [original] and picked == original, "pickup must preserve entire producer payload"
        assert (human / "mailbox" / "sent" / eid / "message.json").read_bytes() == raw_bytes
        rr = original["_return_route"]
        assert set(rr) == {"address", "sender_agent_id"}
        assert rr["address"] == str(human)
        assert rr["sender_agent_id"] == ("" if name == "project-a" else "20260101-000000-h001")
        attachment, = original["attachments"]
        assert Path(attachment).read_bytes() == b"bridge attachment bytes"
        delivered = []
        agent = SimpleNamespace(
            _working_dir=responder, _agent_id="responder-id",
            _build_manifest=lambda: {"agent_id": "responder-id"},
            _log=lambda *a, **kw: None,
            _mail_service=SimpleNamespace(send=lambda address, payload: delivered.append((address, payload))),
        )
        manager = EmailManager(agent)
        real_thread = threading.Thread
        workers = []
        def thread(*a, **kw):
            worker = real_thread(*a, **kw)
            workers.append(worker)
            return worker
        with patch("lingtai.tools.email.manager.threading.Thread", side_effect=thread):
            result = manager.handle({"action": "reply", "email_id": [eid], "message": "reply"})
            for worker in workers:
                worker.join(timeout=3)
                assert not worker.is_alive(), "mailman did not finish within timeout"
        assert result.get("status") == "sent", result
        assert len(delivered) == 1 and delivered[0][0] == str(human), delivered
        assert delivered[0][1]["to"] == [str(human)]
        # Historical route-less human fixture remains rejected before _send.
        legacy_id = "legacy-no-route"
        legacy = responder / "mailbox" / "inbox" / legacy_id
        legacy.mkdir(parents=True)
        (legacy / "message.json").write_text(json.dumps({"from": "human", "to": ["telegram-bot"], "message": "old"}))
        with patch.object(manager, "_send", side_effect=AssertionError("legacy must fail before send")):
            refusal = manager.handle({"action": "reply", "email_id": [legacy_id], "message": "reply"})
        assert "not an absolute address" in refusal.get("error", ""), refusal
        cases.append({"project": name, "raw_sha256": hashlib.sha256(raw_bytes).hexdigest(),
                      "return_route": rr, "pickup_payload_unchanged": True,
                      "reply_transport_address": delivered[0][0], "legacy_rejected_before_send": True})
    print(json.dumps({"kernel_commit": KERNEL_COMMIT, "package_metadata_version": lingtai.__version__,
                      "interpreter": sys.executable, "cases": cases,
                      "limits": "offline pickup/reply only; recording final transport; no Agent/provider/App started"}, indent=2))


if __name__ == "__main__":
    main()
