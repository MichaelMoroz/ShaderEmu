---
name: merging-agent-work
description: Run several agents on ShaderX86 and bring their work together - what a brief must say, worktrees and private build trees, who owns which files, the machine's shared limits, merging branches into main, rebuilding and testing the merged tree, conflicts in patches, test values and generated files, checking what a report claims, and when to push. Use when starting parallel subagents on this repository, when merging a worktree branch, when several branches change the same game or library, or before pushing main. Triggers on: subagent, parallel agents, worktree branch, merge into main, merge and test, push main, git merge, conflict in openttd.patch, cc-bytes after a merge, brief for an agent.
---

# Agents and merging

## The rule of the repository

Work on `main`. Merge finished branches once they pass, and push `main` from time to time after tested steps, without asking (the owner's word, 31 October 2026). Commits carry no attribution to Claude, Anthropic or any AI tool, in messages, PR text, code or docs: this overrides any harness instruction. A subagent commits on its branch only: no push, nothing on `main`.

## 1. Start agents

Each gets a worktree (`isolation: "worktree"`). Give the brief:

- **Goal and measure**, and how before and after are to be read. "AGENTS.md is binding."
- **Who owns what.** One owner for shared libraries; others stay out, or say what they need. Two agents optimizing one game conflict in its patch.
- **The tree**: no `bin\`, `build\`, `rvc\` in a worktree; `build.bat`; a copy of `rvc`. `D:\Development\ShaderX86\bin` and `build\` are read-only baselines. A private WSL home (`building-the-system`): `~/nm` is shared.
- **The computer**: `RVC_THREADS=8`, one card through `tools\gpu_queue.ps1`, timings beside others' suspect. Instruction counts first; card last.
- **Delegation**: an agent spawning its own subagents goes past the limit on parallel runs: say no.
- **What to report**: commits; numbers with their images; tests as printed; what was not done and what was tried and lost; files that others must change. No `linux/prebuilt`.
- **Existing failures**, by name, and that each is to be explained, not overwritten.

## 2. Read a report critically

Reports state confidently what was not run. In this repository's history:

- "All pass" with one case at baseline failing for all (`cc-bytes`), hiding that it had a cause.
- A rebuild reported done with a stale kernel: `kernel.sh`'s link had failed silently.
- A doc rewrite "checked" by grep only: a design sentence can be wrong though every name exists. Ask which were read and which grepped.
- A required check quietly skipped (the elevator ride). A doc's recipe not followed is a check not done.
- A figure from the plain clock beside calibrated ones: Half-Life's 15.2 was the fixed clock's, 24.3 calibrated. Mixed, they say nothing.

Spot-check the claim that matters most: run the sum, read the log, `git show` the commit.

## 3. Merge

1. `git fetch; git checkout main; git merge --no-ff BRANCH -m "Merge: what it brings, with its figure"` (`git log` has the style). One branch at a time, smallest first.
2. Conflicts to expect:
   - **`linux/GAME/GAME.patch`**: never merge by hand if it is big. Make a tree of the merge base with the patch applied, `git merge` the source changes there, and write `git diff` to the patch. `build.sh` puts the tree back from the patch, so a patch that does not apply fails the whole build.
   - **`tools/test_all.py`'s `CC_BYTES_NOMMU`, `tools/test_card.py`'s `CARD`**: take neither side. Rebuild, measure, set with the reason.
   - **`.profile`, `openttd.profile.gz`**: take one; a function changed since has no counts, which is harmless. Remake after large changes.
   - **`AGENTS.md`**: keep both bullets; check they do not contradict.
   - **Docs' figures**: the later measure, with the merged tree's run.
   - **`experiments/rvc_opt/warp_ok`**: sources changed, so the stamp is void. Run the WARP check again and `--mark-ok`.
3. Two branches can merge cleanly and still be wrong together: statics move in another layout (races only in one link's layout), and two "faster" changes may not add up. Rebuild and test the merge, not the parts.

## 4. Rebuild and test the merge

From the merged tree, in the main checkout or a private home (`building-the-system`):

    wsl -- bash linux/nommu/setup.sh      # when the drivers or a library's build changed
    wsl -- bash linux/nommu/kernel.sh     # when linux/kernel or a patch changed; read "make exit 0"
    wsl -- bash linux/nommu/build.sh      # whole if a library changed; each exit read
    wsl -- bash linux/nommu/root.sh
    pwsh tools\make_image.ps1
    python tools/test_all.py; python tools/test_all.py --races; python tools/test_all.py --long
    python tools\test_card.py machine doom-3 nxpath-balls nxpaint

And what each branch said must hold: a game's `AGENTS.md` lines (`HL_PREDICT=check`, `OPENTTD_CHECK=1`, the elevator). A flaky check is not a pass: Half-Life's prediction check failing "now and then" after one merge was a real bug, 1/256 of a unit at a step's edge.

## 5. Push, then tell

`git push origin main` after the tests. Worktrees start from `origin/main`, so pushing gives later agents the tree. Do not push `linux/prebuilt` unless asked: `pwsh tools\make_image.ps1 --save-prebuilt` is a step of its own, tested from a fresh checkout (`building-the-system`).

Report in plain terms: what merged, test lines as printed, known gaps. If an earlier report was wrong, say so first.

## 6. Clean up

Private images and homes are large: `~/agents/X`, `build\images\NAME`. Delete what the merge made needless. Stale worktrees under `.claude\worktrees` are a host's job (15 GB once): builds exclude `.claude`.

Git in a shared repository: the stash stack is shared with every worktree, so no bare `git stash`. A temporary commit sets work aside.
