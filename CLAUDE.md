# Uncharted Android port: instructions for Claude Code

Start every session by reading `docs/HANDOFF.md` (project state, rules, findings, next steps)
and `docs/ANDROID_PORT.md` ("Next steps" and "Debugging").

Rules that always apply:
- Work on branch `android-port`; cherry-pick each code commit onto `main`. `main` must never
  contain the Sony / Naughty Dog launcher art (commits f7da596, c2f1fd0 on `android-port`).
  Never merge `android-port` into `main`; check any cherry-pick range for those commits.
- Never commit signing material (`*.keystore`, passwords). Signing lives in GitHub secrets.
- Commit as `Claude <noreply@anthropic.com>`.
- Record every measurement in `docs/ANDROID_PORT.md` and `CHANGELOG.md`, including settings
  that were tried and rejected.
- The phone (Galaxy S24 Ultra) is reachable with `adb`; the app is `com.uncharted.android`.
