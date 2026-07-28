#!/usr/bin/env python3
"""Push local files to GitHub through the Composio tool-execution API.

Composio redacts the underlying OAuth token, so `git push` over HTTPS is not
possible. Instead we drive the git object model directly:

    create blobs -> create tree -> create commit -> update ref

Usage:
    COMPOSIO_API_KEY=... python3 tools/composio_push.py \
        --branch fix/core-correctness \
        --message "commit message" \
        --base-branch main \
        FILE [FILE ...]
"""

import argparse
import base64
import json
import os
import sys
import time
import urllib.error
import urllib.request

API = "https://backend.composio.dev/api/v3/tools/execute"
OWNER = os.environ.get("GH_OWNER", "archanapriyadarshini1st-debug")
REPO = os.environ.get("GH_REPO", "nextgen-chess-engine")


def call(slug, args, retries=3):
    key = os.environ.get("COMPOSIO_API_KEY")
    if not key:
        sys.exit("COMPOSIO_API_KEY is not set")
    body = json.dumps({"user_id": "default", "arguments": args}).encode()
    req = urllib.request.Request(
        f"{API}/{slug}",
        data=body,
        headers={"x-api-key": key, "Content-Type": "application/json"},
    )
    last = None
    for attempt in range(retries):
        try:
            with urllib.request.urlopen(req, timeout=120) as r:
                out = json.load(r)
            if not out.get("successful"):
                last = str(out.get("error"))[:400]
                # Validation errors will never succeed on retry.
                if "422" in last or "not found" in last.lower():
                    raise RuntimeError(f"{slug}: {last}")
                time.sleep(2 * (attempt + 1))
                continue
            return unwrap(out)
        except urllib.error.HTTPError as e:
            last = f"HTTP {e.code}: {e.read()[:200]!r}"
            time.sleep(2 * (attempt + 1))
        except urllib.error.URLError as e:
            last = str(e)
            time.sleep(2 * (attempt + 1))
    raise RuntimeError(f"{slug} failed after {retries} attempts: {last}")


def unwrap(out):
    """Composio nests the GitHub payload inconsistently; normalise it."""
    d = out.get("data")
    if isinstance(d, dict):
        if "details" in d and isinstance(d["details"], dict):
            return d["details"]
        if "data" in d and isinstance(d["data"], dict):
            return d["data"]
    return d


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--branch", required=True)
    ap.add_argument("--message", required=True)
    ap.add_argument("--base-branch", default="main")
    ap.add_argument("files", nargs="+")
    a = ap.parse_args()

    # Resolve the branch tip, creating the branch from base if it is new.
    try:
        ref = call("GITHUB_GET_A_REFERENCE",
                   {"owner": OWNER, "repo": REPO, "ref": f"heads/{a.branch}"})
        parent = ref["object"]["sha"]
        print(f"branch {a.branch} @ {parent[:8]}")
    except Exception:
        base = call("GITHUB_GET_A_REFERENCE",
                    {"owner": OWNER, "repo": REPO, "ref": f"heads/{a.base_branch}"})
        parent = base["object"]["sha"]
        print(f"branch {a.branch} is new, forking {a.base_branch} @ {parent[:8]}")

    # GITHUB_GET_A_COMMIT returns `details` as an opaque string rather than the
    # commit object, so resolve the base tree via GET_A_TREE instead.
    base_tree = call("GITHUB_GET_A_TREE",
                     {"owner": OWNER, "repo": REPO, "tree_sha": parent})["sha"]

    # Blobs must be uploaded before they can be referenced by a tree.
    tree_entries = []
    for path in a.files:
        if not os.path.isfile(path):
            print(f"  skip (missing) {path}")
            continue
        raw = open(path, "rb").read()
        blob = call("GITHUB_CREATE_A_BLOB", {
            "owner": OWNER, "repo": REPO,
            "content": base64.b64encode(raw).decode(),
            "encoding": "base64",
        })
        tree_entries.append({
            "path": path, "mode": "100644", "type": "blob", "sha": blob["sha"],
        })
        print(f"  blob {blob['sha'][:8]}  {len(raw):>7}  {path}")

    if not tree_entries:
        sys.exit("nothing to push")

    tree = call("GITHUB_CREATE_A_TREE", {
        "owner": OWNER, "repo": REPO,
        "base_tree": base_tree,
        "tree": tree_entries,
    })
    print(f"tree {tree['sha'][:8]}")

    new_commit = call("GITHUB_CREATE_A_COMMIT", {
        "owner": OWNER, "repo": REPO,
        "message": a.message,
        "tree": tree["sha"],
        "parents": [parent],
    })
    print(f"commit {new_commit['sha'][:8]}")

    try:
        call("GITHUB_UPDATE_A_REFERENCE", {
            "owner": OWNER, "repo": REPO,
            "ref": f"heads/{a.branch}",
            "sha": new_commit["sha"],
        })
    except Exception:
        call("GITHUB_CREATE_A_REFERENCE", {
            "owner": OWNER, "repo": REPO,
            "ref": f"refs/heads/{a.branch}",
            "sha": new_commit["sha"],
        })
    print(f"pushed {new_commit['sha'][:8]} -> {a.branch}")


if __name__ == "__main__":
    main()
