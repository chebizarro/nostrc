---
tags: [gnome-hig, copy]
max_turns: 20
timeout_seconds: 600
allowed_tools: [Read, Glob, Grep, Skill, Write, Edit, Bash]
---

The strings in this export dialog read wrong for a GNOME app — a reviewer said the wording and labels "don't feel native" but didn't say what to change. Can you fix the text and labels so it matches what GNOME expects? Keep the widget structure as it is and change only what's needed (wording, capitalization, punctuation, button labels/order, tooltips, accessibility labels if any are missing). Write the corrected file as export-dialog.ui and put a list of every change with a one-line reason in CHANGES.md.

Input files: export-dialog.ui
