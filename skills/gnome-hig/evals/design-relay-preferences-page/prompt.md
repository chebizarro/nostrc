---
tags: [gnome-hig, design]
max_turns: 30
timeout_seconds: 900
allowed_tools: [Read, Glob, Grep, Skill, Write, Edit, Bash]
---

I'm adding a Relays page to gnostr's preferences dialog. It needs: the list of the user's relays (show the URL, let them remove any relay and add a new one by typing a wss:// URL), per-relay Read and Write toggles, a "connection timeout" in seconds (1 to 120), and an option to automatically reconnect when a relay drops. Tell me which libadwaita widgets to use for each part and why (I keep reaching for GtkBox + GtkButton and it never looks right), then write the Blueprint UI definition for the page as relays-page.blp and a short DESIGN.md with the rationale. Don't wire up the C code, just the UI definition and the reasoning.
