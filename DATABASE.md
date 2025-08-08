# Database

This project does not use a database. It produces C libraries intended to be embedded into other applications. Any persistence or data storage is the responsibility of the embedding application.

## If You Introduce Storage Later

- Prefer pluggable interfaces to avoid hard dependencies.
- Document schemas and migration strategies here if a database is added.
- Consider simple file-based formats (e.g., SQLite) for portability if needed by examples/tools.
