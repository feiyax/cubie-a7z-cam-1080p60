# Source history, builds and agent metadata

## Reviewable upstream baseline

The existing human-created empty foundation commit is preserved. The next commit imports an **exact scoped upstream baseline**, not the entire BSP: the two touched BSP source files at their final paths, the Intel mode reference, and the required Khronos OpenMAX headers. `upstream/manifest.json` records pinned commits, URLs and original byte hashes. Upstream notices, line endings and even existing whitespace are not rewritten in that import. `.gitattributes` prevents automatic line-ending conversion of these sources.

Later commits isolate sensor changes, VIN changes, the OMX EOS adapter, the ISP adapter, and supporting tools/docs. Thus `git log -p -- kernel/drivers/vin/modules/sensor/imx214.c` shows the exact vendor file followed by local changes. There is no imported private development history or claim that the entire vendor BSP is hosted here.

Verify the original import and current immutable references:

```sh
baseline=$(git log --reverse --format=%H -- upstream/manifest.json | head -1)
python3 scripts/fetch-upstream.py --baseline-ref "$baseline"
```

The build downloads the pinned BSP archive to an ignored cache, verifies SHA256 and the two original file hashes, then overlays `kernel/`. It regenerates the sensor table and compares it to the tracked file before compiling. Each build uses new copied kernel headers and leaves its log under `.work/`. Delete only your own disposable build directories when finished. Matching kernel release alone is not a substitute for matching configuration/Module.symvers or real hardware validation.

## Commit policy

Every new commit includes explicit agent/model provenance. The sole preexisting exception is the human foundation `ca58666dcf7403f12fa985abc9fcec9596d6b186`. Do not rewrite it. No global Git configuration or hooks are required.

Required trailers:

```text
Agent-Name: Codex /root/camera_1080p60_rework
Model-Name: GPT-6
Model-Variant: Astra
Model-ID: gpt-6-astra
Reasoning-Effort: high
Model-Version: not-exposed
```

These describe the lead implementer for this work. The exact serving model snapshot/version was not exposed; `not-exposed` is deliberate, not a version inferred from an alias. Medium-reasoning reviewers contributed review and documentation; the committing agent records its own actual identity. Future agents must supply their own truthful fields rather than copy this example as an assertion about themselves.

Use an explicit agent author and committer, not a user's inherited Git identity. The helper requires every field and commits only already staged changes:

```sh
python3 scripts/agent-commit.py --message-file /path/to/message.txt \
  --agent-name 'YOUR ACTUAL AGENT NAME' --agent-email 'agent@example.invalid' \
  --model-name 'YOUR MODEL FAMILY' --model-variant 'YOUR VARIANT' \
  --model-id 'YOUR MODEL ID' --reasoning-effort high --model-version not-exposed
```

An optional local hook can be enabled with `git config --local core.hooksPath .githooks`. CI fetches full history and checks all new commits, including author/committer consistency. Run `python3 scripts/check-commit-metadata.py --range HEAD` yourself. The host test includes a missing-trailer rejection and a valid explicit-identity case. No helper pushes or changes remote configuration.

## Checks and limits

Run `scripts/test-host.sh` and `scripts/build-userspace.sh`; build modules against matching headers as in the README. `scripts/scrub-public.py` rejects common accidental private paths/network addresses and generated binary/media files in tracked or nonignored candidate files. It supplements manual privacy/license review; it cannot prove that arbitrary text contains no sensitive information.

Kernel builds, proprietary SDK audit builds, and hardware tests are not executed by ordinary GitHub host CI. The hardware evidence summarized in [validation.md](validation.md) was obtained before this public packaging step. Public packaging was tested with a fresh isolated kernel build and ARM64 userspace build, without changing the board runtime. Requalification is required after meaningful changes to sensor, VIN, vendor ABI or pipeline behavior.
