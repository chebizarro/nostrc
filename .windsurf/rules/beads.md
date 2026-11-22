# Beads Issue Tracking

This project uses [Beads (bd)](https://github.com/steveyegge/beads) for issue tracking.
[`AGENTS.md`](../../AGENTS.md) is the single source of truth for the bead lifecycle:
when to file a bead (search active and closed beads first), when an implementation
bead closes, what a close reason must cite, and how to hand off or release claims.
Follow it; this file does not add or override policy.

## Quick Reference

```bash
bd prime                              # Load current command help
bd ready                              # Show issues ready to work (no blockers)
bd search <text>                      # Search before filing a new bead
bd show <id>                          # Read scope and current state
bd update <id> --claim                # Claim before editing
bd close <id> --reason "..."          # Cite a reachable commit, keeper id or decision
```

## Priorities

- `0` - Critical (security, data loss, broken builds)
- `1` - High (major features, important bugs)
- `2` - Medium (default)
- `3` - Low (polish, optimization, optional hardening)
- `4` - Backlog (future ideas)
