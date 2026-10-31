#!/usr/bin/env bash
cd "$CLAUDE_PROJECT_DIR" || exit 0
[ -f DONE ] && exit 0
if [ -f .claude/waiting-for-team ]; then rm -f .claude/waiting-for-team; exit 0; fi
echo "Pas de fichier DONE : les critères de réussite ne sont pas remplis. Relis PROGRESS.md et continue. Si tu attends des coéquipiers qui travaillent, crée .claude/waiting-for-team puis termine ton tour." >&2
exit 2
