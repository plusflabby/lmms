# PR A: Bind copied project identity to the accepted Windows pipe

Base: committed Phase 3J-R native `aeba0e0ed13f5963628c3564b608f50effe9b809`. Integrates exactly the four identity commits: eb84cfed4, b88a0662f, 8e806366c and 77a11536d. They applied without source conflicts. No uncommitted recording changes were copied.

Expose bounded copied loaded-project path/file/hash/document-state evidence and accepted pipe server/client PIDs with kernel creation times. Preserve legacy response compatibility, saved/dirty/recovered distinctions, and Save As/reload invalidation. This snapshot does not provide an atomic expected-song mutation guard or authorize DAW edits.

Diff against the intended base: eight identity-related files, no deleted files. The Recording headers, src/core/recording and recording tests are byte-identical to the committed base; the source changes in Song preserve the recording additions. Existing Phase 3J-R inspection and recording code stays present.

Verification: a new Debug/Ninja build under build-pr-identity compiled 428 steps from this clean source with MSYS2 GCC 16.2.0. Targeted identity plus broader vocal/recording CTest passed 16/16 in 59.18 seconds: pipe/context/protocol/journal, vocal safety/jobs/analysis/source/types, streaming PCM, output queue, session state, library, project identity, mono ring and PCM16 conversion. Tests and logs are in the isolated build directory. The entire native suite was not built/run; no LMMS application or live acceptance session was launched.

Dependencies: no fetched remote contains aeba0e0. Publish/review the committed Phase 3J-R native prerequisite under separate authorization before opening this stacked PR against it. Dirty P2 RecordingRenderLease changes remain outside this branch; eventual integration with that later committed work must preserve both changes to Song.cpp. Review was performed inline by the author; independent PR review is still required.

PR D remains a plan only: after this PR is actually merged and remotely verified, update only the parent lmms gitlink to that actual merge SHA in a separate change. Do not substitute this feature SHA for a future merge commit.
