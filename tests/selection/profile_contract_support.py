"""Source job/matrix accounting, not a general YAML parser or execution receipt.

Actionlint owns YAML/Actions semantics. Keeping the literal strategy matrix means
static, include/exclude and dynamically supplied matrices cannot silently change
without updating their supported mapping or their explicitly incomplete record.
"""
from pathlib import Path
import re


def source_jobs(root):
    jobs = {}
    for path in sorted((Path(root) / ".github/workflows").iterdir()):
        if path.suffix not in {".yml", ".yaml"}:
            continue
        lines = path.read_text().splitlines()
        start = lines.index("jobs:") + 1
        headers = []
        for i, line in enumerate(lines[start:], start):
            if not line.strip() or line.lstrip().startswith("#"):
                continue
            if len(line) - len(line.lstrip()) != 2:
                continue
            header = re.fullmatch(r'''  (?:([A-Za-z0-9_-]+)|"([A-Za-z0-9_-]+)"|'([A-Za-z0-9_-]+)'):\s*(?:#.*)?''', line)
            if not header:
                raise ValueError("unsupported job header: " + str(path) + ":" + str(i + 1))
            headers.append((i, next(value for value in header.groups() if value)))
        for number, (first, name) in enumerate(headers):
            end = headers[number + 1][0] if number + 1 < len(headers) else len(lines)
            block = lines[first:end]
            matrices = []
            in_strategy = False
            for i, line in enumerate(block):
                if not line.strip() or line.lstrip().startswith("#"):
                    continue
                indent = len(line) - len(line.lstrip())
                if indent == 4:
                    in_strategy = False
                if indent != 4 and not (indent == 6 and in_strategy):
                    continue
                field = re.fullmatch(r'''\s*(?:([A-Za-z0-9_-]+)|"([A-Za-z0-9_-]+)"|'([A-Za-z0-9_-]+)'):\s*(.*)''', line)
                if not field:
                    raise ValueError("unsupported job/strategy syntax: " + str(path) + ":" + name)
                key = next(value for value in field.groups()[:3] if value)
                value = field[4]
                if indent == 4 and key == "strategy":
                    if value and not value.startswith("#"):
                        raise ValueError("inline strategy requires explicit accounting support: " + str(path) + ":" + name)
                    in_strategy = True
                if indent == 6 and key == "matrix":
                    matrices.append(i)
            if len(matrices) > 1:
                raise ValueError("multiple matrix declarations: " + str(path) + ":" + name)
            matrix = []
            if matrices:
                i = matrices[0]
                matrix.append(block[i].strip())
                for line in block[i + 1:]:
                    if line.strip() and len(line) - len(line.lstrip()) <= 6:
                        break
                    matrix.append(line.rstrip())
                while matrix and not matrix[-1].strip():
                    matrix.pop()
            key = path.relative_to(root).as_posix() + ":" + name
            jobs[key] = "\n".join(matrix)
    return jobs


def validate_job_accounting(policy, root):
    observed = source_jobs(root)
    records = policy["workflow_jobs"]
    if set(observed) != set(records):
        raise ValueError("unaccounted/stale workflow jobs: " + str(sorted(set(observed) ^ set(records))))
    gaps = {gap["id"] for gap in policy["coverage_gaps"]}
    for key, matrix in observed.items():
        record = records[key]
        if record["matrix_source"] != matrix:
            raise ValueError("workflow matrix changed: " + key)
        if not record.get("profiles") and not record.get("coverage_gap"):
            raise ValueError("job has neither profiles nor an explicit gap: " + key)
        if record.get("coverage_gap") and record["coverage_gap"] not in gaps:
            raise ValueError("untracked profile gap: " + key)
        for profile_id in record.get("profiles", []):
            profile = policy["profiles"][profile_id]
            if profile["setup"] != key:
                raise ValueError("profile/job identity mismatch: " + profile_id)
    return observed


def step_script(text, name):
    """Extract one literal run block at the repository's fixed step indentation."""
    header = "      - name: " + name
    lines = text.splitlines()
    first = lines.index(header) + 1
    end = next((i for i in range(first, len(lines)) if lines[i].startswith("      - ")), len(lines))
    run = next(i for i in range(first, end) if lines[i].startswith("        run: "))
    value = lines[run].removeprefix("        run: ")
    if value != "|":
        return value + "\n"
    body = []
    for line in lines[run + 1:end]:
        if line and not line.startswith("          "):
            break
        body.append(line[10:] if line else "")
    return "\n".join(body).rstrip() + "\n"
