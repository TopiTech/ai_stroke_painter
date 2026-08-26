# Taste

## Communication
- User communicates in Japanese (e.g., "作業再開"); respond in Japanese. Project comments/docstrings in their codebase are also written in Japanese. Confidence: 0.9

## Environment / Tooling
- Development environment is Windows: the shell lacks Unix utilities (`tail`, etc.), so avoid piping command output through them — run commands directly instead. File paths use backslashes (e.g., `C:\Users\mibu0\...`). Confidence: 0.85
- Invokes the interpreter as `python` (not `python3`). Confidence: 0.8

## Workflow / Code Review
- Prefers structured code-review lifecycle: survey whole codebase for quality/maintainability/security/performance → prioritize by severity/impact → fix important issues with minimal behavior changes → verify with tests/quality checks → report as 問題点/変更内容/テスト結果/未解決事項. Tracked explicitly via todo phases. Confidence: 0.88
- Expects comprehensive verification via `python check.py` (Ruff/Format/Mypy/Pyrefly/quality gates/regression) to reach ALL CHECKS PASSED, with auto-fix via `python -m ruff check --fix . && python -m ruff format .` and status checks via `git status --short` before resuming work. Confidence: 0.85
