# Public release review

Review date: 2026-09-12.

## Sensitive information

- Gitleaks 8.30.1 scanned the working directory and the complete existing Git history with redacted reporting: no findings.
- `tools/secret_scan.py` checked 15 known-value SHA-256 fingerprints: no matches. This is a targeted regression check, not proof that every possible secret is absent.
- Reviewed credential examples, service configuration, deployment scripts, local-path references, and preview images. Published network addresses, tokens, and PINs are placeholders or synthetic fixtures.
- Live credential headers, environment files, databases, configured firmware, and build outputs are excluded. SQLite journal/backup patterns are now ignored as well.
- Commit attribution uses a GitHub noreply email address.

## Documentation and previews

- Updated English and Chinese READMEs and the agent guide. Clarified the reference ESP-IDF version versus the manifest range, dependency-patch order, environment loading, tracked relay configuration, and board-porting boundaries.
- Exported six 800×480 screens from the actual LVGL host renderer, plus an overview. The screenshots use synthetic fixtures. Reproduction instructions are in `assets/preview/README.md`.
- Retained and verified the pre-existing local completion-label font adjustment and its host assertions.

## Validation

- Actual LVGL host smoke verification: passed, including layout, interaction, parsing, notifications, and video interruption paths.
- RTP reassembly, Todo receipt handling, and latest-IDR host checks: passed.
- Todo/Feishu tests: passed.
- Video relay tests: 42 passed, 0 failed. ffmpeg decoding and live-printer checks were skipped because their runtime requirements were not available/configured.
- Open-package checks: passed (12 tests).
- README/gallery local links and Git whitespace checks: passed.

This review does not include a new firmware build, physical-device flashing, or a live printer/service deployment.
