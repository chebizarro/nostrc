"""Bound current CMake/Ninja data generators, never arbitrary custom shell.

The source declaration, native argv and declared dependency/output sets must
agree. Tool-specific readers/writers account for resource/schema/locale directory
inputs and the narrow copy side effects used by those producers. Unknown tools,
options, interpreters, dynamic dependencies and source-tree writes refuse.
"""
import ast
from collections import Counter
import hashlib
import json
import os
from pathlib import Path
import re
import shlex
import shutil
import subprocess
import xml.etree.ElementTree as ET


class ProducerError(ValueError):
    pass


def hashed(path):
    return hashlib.sha256(Path(path).read_bytes()).hexdigest()


def tree_fingerprints(directory):
    # Python may execute a valid cached module without reading its .py source.
    return {str(p.resolve()): hashed(p) for p in sorted(directory.rglob('*')) if p.is_file()}


def absolute(base, value):
    return os.path.abspath(os.path.join(str(base), value))


def shell_commands(command):
    if any(c in command for c in ('$', '`', '\n', '\r', '\x00', '*', '?', '[', ']', '{', '}', '~', '(', ')', '#')):
        raise ProducerError('custom shell expansion is not a supported producer')
    lexer = shlex.shlex(command, posix=True, punctuation_chars=';&|<>')
    lexer.whitespace_split, lexer.commenters = True, ''
    chunks = [[]]
    for word in lexer:
        if word == '&&':
            if not chunks[-1]:
                raise ProducerError('empty custom command')
            chunks.append([])
        elif word and all(char in ';&|<>' for char in word):
            raise ProducerError('custom shell operator is not a supported producer')
        else:
            chunks[-1].append(word)
    if not chunks[-1] or len(chunks[0]) != 2 or chunks[0][0] != 'cd':
        raise ProducerError('custom command requires one literal CMake working directory')
    return chunks[0][1], chunks[1:]


def declarations(realization):
    root, build = Path(realization['repo_root']), Path(realization['build_dir'])
    context = realization['default_configure_context']
    source_root = Path(realization['source_dir'])
    directories = {str(source_root / d['source'] / 'CMakeLists.txt'): (str(source_root / d['source']), str(build / d['build']))
                   for d in context['directories']}
    inputs = {str(Path(p)): h for p, h in context['source_inputs'].items()}
    for path, expected in inputs.items():
        if not Path(path).is_file() or Path(path).resolve() != Path(path) or hashed(path) != expected:
            raise ProducerError('configure source input changed: ' + path)
    frames, custom, utilities, literals, written, dependencies = {}, [], {}, {}, {}, {}
    keywords = {'OUTPUT', 'BYPRODUCTS', 'COMMAND', 'DEPENDS', 'MAIN_DEPENDENCY', 'WORKING_DIRECTORY',
                'COMMENT', 'VERBATIM', 'USES_TERMINAL', 'COMMAND_EXPAND_LISTS', 'ALL'}
    for line in (build / 'nostrc-configure-trace.jsonl').read_text().splitlines():
        event = json.loads(line)
        file, cmd, args = event.get('file', ''), event.get('cmd', '').lower(), event.get('args', [])
        depth = event.get('global_frame', 0)
        if file in directories:
            frames = {k: v for k, v in frames.items() if k <= depth}
            frames[depth] = directories[file]
        if cmd in {'function', 'macro'} and args and args[0].lower() in {'add_custom_command', 'add_custom_target', 'file', 'configure_file'}:
            # The existing configure-output witness helper defines no overrides.
            raise ProducerError('overridden generator provenance builtin: ' + args[0])
        if file not in inputs:
            continue
        scope = frames[max((k for k in frames if k <= depth), default=0)] if frames else None
        if scope is None:
            continue
        source, binary = scope
        witness = {'file': file, 'line': event['line'], 'sha256': inputs[file]}
        # Literal metadata is validated against expected CONTENT/WRITE bytes,
        # not attributed merely because some generated file happens to exist.
        if file == str(root / 'cmake/NostrcGtkResources.cmake') and cmd == 'file' and args[:2] == ['GENERATE', 'OUTPUT']:
            if len(args) != 5 or args[3] != 'CONTENT' or '$<' in args[4] or not args[2].endswith('-resource-inputs.txt'):
                raise ProducerError('unsupported resource lookup metadata declaration')
            output = absolute(binary, args[2])
            if output in literals:
                raise ProducerError('ambiguous literal generator metadata')
            literals[output] = dict(witness, expected_sha256=hashlib.sha256(args[4].encode()).hexdigest())
        if file == str(root / 'cmake/NostrcGettext.cmake') and cmd == 'file' and args[:1] == ['WRITE']:
            if len(args) == 3 and args[1].endswith('/po/LINGUAS.tmp') and re.fullmatch(r'[\w@ .\-\n]*', args[2]):
                output = absolute(binary, args[1])
                written[(depth, event.get('frame'))] = (output, args[2], witness)
        elif file == str(root / 'cmake/NostrcGettext.cmake') and cmd == 'configure_file':
            previous = written.pop((depth, event.get('frame')), None)
            if previous and args == [previous[0], previous[0][:-4], 'COPYONLY']:
                for output in (args[0], args[1]):
                    if output in literals:
                        raise ProducerError('ambiguous locale lookup metadata')
                    literals[output] = dict(previous[2], expected_sha256=hashlib.sha256(previous[1].encode()).hexdigest())
        elif file == str(root / 'cmake/NostrcGettext.cmake'):
            written.pop((depth, event.get('frame')), None)
        if cmd == 'add_dependencies' and len(args) > 1:
            dependencies.setdefault(args[0], []).append(dict(witness, targets=args[1:]))
        if cmd not in {'add_custom_command', 'add_custom_target'}:
            continue
        data = dict(witness, source=source, binary=binary, commands=[], outputs=[], depends=[], byproducts=[], working_directory=binary)
        # CMake expands list-valued COMMAND variables into semicolon lists in
        # the JSON trace; list expansion occurs before directive parsing.
        rest = [item for arg in args for item in arg.split(';')]
        if cmd == 'add_custom_target':
            data['name'], rest = rest[0], rest[1:]
        field = None
        try:
            for value in rest:
                if value in {'DEPFILE', 'IMPLICIT_DEPENDS', 'APPEND', 'TARGET', 'PRE_BUILD', 'PRE_LINK', 'POST_BUILD', 'JOB_POOL', 'JOB_SERVER_AWARE'}:
                    raise ProducerError('unadmitted dynamic/hook custom directive: ' + value)
                if value in keywords:
                    field = value
                    if field == 'COMMAND': data['commands'].append([])
                    continue
                if field == 'COMMAND': data['commands'][-1].extend(value.split(';'))
                elif field in {'OUTPUT', 'BYPRODUCTS', 'DEPENDS', 'MAIN_DEPENDENCY'}:
                    key = {'OUTPUT': 'outputs', 'BYPRODUCTS': 'byproducts', 'MAIN_DEPENDENCY': 'depends', 'DEPENDS': 'depends'}[field]
                    data[key].extend(value.split(';'))
                elif field == 'WORKING_DIRECTORY': data['working_directory'] = absolute(binary, value)
                elif field != 'COMMENT': raise ProducerError('unsupported CMake custom directive: ' + value)
            if any('$<' in x for key in ('outputs', 'byproducts', 'depends') for x in data[key]):
                raise ProducerError('dynamic custom dependency/output expression')
            data['outputs'] = sorted({absolute(binary, x) for x in data['outputs'] + data['byproducts']})
        except ProducerError as exc:
            data['error'] = str(exc)
        if cmd == 'add_custom_target':
            utilities.setdefault(data['name'], []).append(data)
        else:
            custom.append(data)
    for path, proof in literals.items():
        if not Path(path).is_relative_to(build) or Path(path).resolve() != Path(path) or not Path(path).is_file() or hashed(path) != proof['expected_sha256']:
            raise ProducerError('literal configure metadata bytes/origin differ: ' + path)
    for name, specs in utilities.items():
        for spec in specs:
            spec['target_dependencies'] = dependencies.get(name, [])
    return custom, utilities, literals


def native_commands(snapshot, build):
    result = {}
    for name in snapshot['manifests']['files']:
        text = Path(name).read_text().replace('$\r\n', '').replace('$\n', '')
        for block in text.split('\n\n'):
            first = next((line for line in block.splitlines() if line.startswith('build ') and ': CUSTOM_COMMAND' in line), None)
            if not first:
                continue
            output_text = first[6:].split(': CUSTOM_COMMAND', 1)[0]
            # Preserve a substituted build prefix as one literal Ninja path.
            prefix = str(build).replace(' ', '\x01') + '/'
            output_text = output_text.replace('${cmake_ninja_workdir}', prefix).replace('$cmake_ninja_workdir', prefix)
            # CMake escapes literal path punctuation with Ninja's dollar escape.
            output_text = re.sub(r'\$([ :$])', lambda m: {' ': '\x01', ':': ':', '$': '$'}[m[1]], output_text)
            if '$' in output_text:
                raise ProducerError('unknown native custom output variable')
            outputs = {absolute(build, x.replace('\x01', ' ')) for x in output_text.split() if x != '|'}
            commands = [line[len('  COMMAND = '):] for line in block.splitlines() if line.startswith('  COMMAND = ')]
            if len(commands) != 1:
                continue  # A reachable commandless/ambiguous rule will refuse below.
            command = re.sub(r'\$([ :$])', lambda m: m[1], commands[0])
            for path in outputs:
                if path in result and result[path] != (outputs, command):
                    raise ProducerError('ambiguous native custom output')
                result[path] = (outputs, command)
    return result


def attest(realization, snapshot, run):
    root, build = Path(realization['repo_root']), Path(realization['build_dir'])
    env = run.environment
    custom, utilities, literals = declarations(realization)
    native = native_commands(snapshot, build)
    all_native = {absolute(build, p) for p, e in snapshot['edges'].items() if e['rule'] != 'phony'}
    known = all_native | set(literals) | set(realization['configure_outputs'])
    tools = {p: hashed(p) for p in realization['default_configure_context']['external_inputs']}
    inputs, outputs, proofs, admitted, gaps = set(), set(), {}, set(), []
    targets = realization['targets']
    query_targets, query_commands, queried = [], [], set()
    for name, edge in snapshot['edges'].items():
        path = absolute(build, name)
        if edge['rule'] == 'CUSTOM_COMMAND' and path in native and path not in queried:
            group, command = native[path]
            queried.update(group)
            query_targets.append(name)
            query_commands.append(command)
    if query_targets:
        actual = run(['ninja', '-C', str(build), '-t', 'commands', '-s', *query_targets])
        if Counter(actual.splitlines()) != Counter(query_commands):
            raise ProducerError('native custom command query differs from bound manifest')

    def file_input(path, produced=()):
        path = str(Path(path))
        p = Path(path)
        if p.is_relative_to(build):
            if path not in known and path not in outputs and path not in produced:
                raise ProducerError('generated reader has no bound producer: ' + path)
        elif not p.is_relative_to(root) or not p.is_file() or p.resolve() != p:
            raise ProducerError('producer input escapes source or is absent/symlinked: ' + path)
        inputs.add(path)
        return path

    def output(path):
        p = Path(path)
        if not p.is_relative_to(build) or p.resolve(strict=False) != p:
            raise ProducerError('producer output escapes build or traverses a symlink: ' + path)
        return path

    def tool(path):
        p = Path(path)
        if not p.is_absolute():
            if p.name != path:
                raise ProducerError('relative generator executable path: ' + path)
            found = shutil.which(path, path=env.get('PATH'))
            if not found:
                raise ProducerError('missing PATH generator executable: ' + path)
            p = Path(found)
        if not p.is_file() or not os.access(p, os.X_OK):
            raise ProducerError('missing generator executable: ' + path)
        resolved = p.resolve()
        actual = shutil.which(p.name, path=env.get('PATH'))
        if (resolved.is_relative_to(root) or resolved.is_relative_to(build) or not actual
                or Path(actual).resolve() != resolved
                or (p.name == 'cmake' and str(resolved) != snapshot['build_tools']['cmake']['path'])):
            raise ProducerError('generator is not the configured/PATH external tool: ' + path)
        if str(resolved) not in tools:
            tools[str(resolved)] = hashed(resolved)
            first = p.read_bytes().split(b'\n', 1)[0]
            if first.startswith(b'#!'):
                interpreter = first[2:].decode().strip().split()
                if len(interpreter) == 2 and interpreter[0] == '/usr/bin/env' and re.fullmatch(r'python3(?:\.\d+)?', interpreter[1]):
                    tools['/usr/bin/env'] = hashed('/usr/bin/env')
                    selected = shutil.which(interpreter[1], path=env.get('PATH'))
                    if not selected:
                        raise ProducerError('missing PATH generator Python interpreter')
                    interpreter = [selected]
                if len(interpreter) != 1 or not Path(interpreter[0]).is_absolute() or not re.fullmatch(r'python3?(?:\.\d+)?', Path(interpreter[0]).name):
                    raise ProducerError('unsupported generator script interpreter')
                tools[str(Path(interpreter[0]).resolve())] = hashed(interpreter[0])
                if any(env.get(k) for k in ('PYTHONHOME', 'PYTHONPATH', 'PYTHONPYCACHEPREFIX', 'UNINSTALLED_GLIB_SRCDIR', 'GI_TYPELIB_PATH')):
                    raise ProducerError('generator Python/import path override is not admitted')
                if p.name == 'gdbus-codegen':
                    package = p.parent.parent / 'share/glib-2.0/codegen'
                    if not package.is_dir(): raise ProducerError('gdbus package is not at its bound relocatable location')
                    bind_tree(package)
                elif p.name == 'blueprint-compiler':
                    launcher = ast.parse(p.read_text())
                    insertions = [node for node in ast.walk(launcher) if isinstance(node, ast.Call)
                                  and ast.unparse(node.func).startswith('sys.path.')]
                    module_paths = [node.value.value for node in launcher.body if isinstance(node, ast.Assign)
                                    and any(isinstance(t, ast.Name) and t.id == 'module_path' for t in node.targets)
                                    and isinstance(node.value, ast.Constant) and isinstance(node.value.value, str)]
                    if insertions:
                        # Meson's installed 0.12+ launcher puts its literal
                        # module_path first, which can be outside site-packages.
                        if (len(insertions) != 1 or ast.unparse(insertions[0]) != 'sys.path.insert(0, module_path)'
                                or len(module_paths) != 1 or not Path(module_paths[0]).is_absolute()):
                            raise ProducerError('unsupported Blueprint launcher package path')
                        package_paths = [str(Path(module_paths[0]) / 'blueprintcompiler')]
                    else:
                        script = 'import importlib.util,json; s=importlib.util.find_spec("blueprintcompiler"); print(json.dumps(list(s.submodule_search_locations) if s else []))'
                        package_paths = json.loads(run([interpreter[0], '-I', '-c', script]))
                    if len(package_paths) != 1: raise ProducerError('ambiguous Blueprint tool package')
                    bind_tree(Path(package_paths[0]))
                    bind_tree(p.parent.parent / 'share/gir-1.0', optional=True)
                    bind_tree(Path('/usr/share/gir-1.0'), optional=True)
                    for directory in (p.parent.parent / 'lib/girepository-1.0', Path('/usr/lib/girepository-1.0'), Path('/usr/lib/x86_64-linux-gnu/girepository-1.0')):
                        bind_tree(directory, optional=True)
        return p.name

    def bind_tree(directory, optional=False):
        if not directory.is_dir():
            if optional: return
            raise ProducerError('missing generator data/package directory: ' + str(directory))
        tools.update(tree_fingerprints(directory))

    def patharg(cwd, value): return absolute(cwd, value)

    def io_commands(commands, cwd, declared, native_inputs):
        reads, writes = set(), set()
        for argv in commands:
            if not argv: raise ProducerError('empty generator argv')
            name = tool(argv[0]); a = argv[1:]
            def read(value, base=cwd):
                p = patharg(base, value)
                if p not in native_inputs and p not in writes and p not in literals:
                    raise ProducerError('generator reads undeclared input before producing it: ' + p)
                reads.add(p); file_input(p, writes); return p
            def write(value):
                p = patharg(cwd, value); writes.add(p); output(p); return p
            if name == 'cmake':
                if a[:2] == ['-E', 'make_directory'] and len(a) >= 3:
                    for value in a[2:]:
                        p = Path(patharg(cwd, value))
                        if not p.is_relative_to(build) or p.resolve(strict=False) != p: raise ProducerError('generator mkdir escapes build')
                elif a[:2] == ['-E', 'copy'] and len(a) >= 4:
                    sources = [read(x) for x in a[2:-1]]; dest = patharg(cwd, a[-1])
                    directory = len(sources) > 1 or Path(dest).is_dir() or (dest not in declared and not Path(dest).suffix)
                    for source in sources: write(str(Path(dest) / Path(source).name) if directory else dest)
                else: raise ProducerError('unsupported cmake custom operation')
            elif name == 'blueprint-compiler':
                if len(a) != 4 or a[:2] != ['compile', '--output'] or not a[3].endswith('.blp'): raise ProducerError('unsupported Blueprint argv')
                write(a[2]); read(a[3])
            elif name == 'gdbus-codegen':
                options, xml = {}, []
                i = 0
                while i < len(a):
                    if a[i] in {'--generate-c-code', '--c-namespace', '--interface-prefix'} and i + 1 < len(a):
                        if a[i] in options: raise ProducerError('duplicate gdbus option')
                        options[a[i]] = a[i+1]; i += 2
                    else: xml.append(a[i]); i += 1
                if set(options) - {'--generate-c-code', '--c-namespace', '--interface-prefix'} or '--generate-c-code' not in options or len(xml) != 1 or xml[0].startswith('-'):
                    raise ProducerError('unsupported gdbus argv')
                xml_path = read(xml[0])
                if any(marker in Path(xml_path).read_text() for marker in ('<!DOCTYPE', '<!ENTITY')):
                    raise ProducerError('external gdbus XML declaration')
                write(options['--generate-c-code'] + '.c'); write(options['--generate-c-code'] + '.h')
            elif name == 'glib-compile-resources':
                options, flags, xml = {}, set(), []; i = 0
                while i < len(a):
                    key, equals, value = a[i].partition('=')
                    if key in {'--target', '--sourcedir', '--c-name'}:
                        if not equals:
                            i += 1
                            if i == len(a): raise ProducerError('missing GResource option value')
                            value = a[i]
                        options.setdefault(key, []).append(value)
                    elif a[i] in {'--generate-source', '--manual-register'}: flags.add(a[i])
                    else: xml.append(a[i])
                    i += 1
                if flags - {'--generate-source', '--manual-register'} or '--generate-source' not in flags or len(xml) != 1 or xml[0].startswith('-') or len(options.get('--target', [])) != 1:
                    raise ProducerError('unsupported GResource argv')
                xml_path = read(xml[0]); write(options['--target'][0])
                text = Path(xml_path).read_text()
                if '<!DOCTYPE' in text or '<!ENTITY' in text: raise ProducerError('external resource XML declaration')
                for node in ET.fromstring(text).iter('file'):
                    if not node.text: raise ProducerError('empty resource input')
                    candidates = [patharg(cwd, str(Path(d) / node.text)) for d in options.get('--sourcedir', [])]
                    candidates += [patharg(Path(xml_path).parent, node.text)]
                    selected = next((p for p in candidates if p in known or Path(p).is_file()), None)
                    if selected is None: raise ProducerError('resource input has no declared producer: ' + node.text)
                    read(selected)
                    for preprocess in node.get('preprocess', '').split(','):
                        if not preprocess: continue
                        if preprocess != 'xml-stripblanks': raise ProducerError('unsupported resource preprocessor: ' + preprocess)
                        if env.get('XMLLINT'): raise ProducerError('resource XMLLINT override is not admitted')
                        helper = shutil.which('xmllint', path=env.get('PATH'))
                        if helper: tool(helper)
            elif name == 'glib-compile-schemas':
                strict = '--strict' in a; values = [x for x in a if x != '--strict']
                dirs = [x for x in values if not x.startswith('--targetdir=')]
                dests = [x.split('=', 1)[1] for x in values if x.startswith('--targetdir=')]
                if len(dirs) != 1 or len(dests) > 1 or dirs[0].startswith('-'): raise ProducerError('unsupported schema argv')
                directory = Path(patharg(cwd, dirs[0])); dest = patharg(cwd, dests[0]) if dests else str(directory)
                schema_inputs = {p for p in writes | native_inputs if Path(p).parent == directory and p.endswith(('.gschema.xml', '.gschema.override'))}
                existing = {str(p) for p in directory.glob('*') if p.is_file() and p.name.endswith(('.gschema.xml', '.gschema.override'))}
                if not schema_inputs or existing - schema_inputs: raise ProducerError('schema directory has unbound inputs: ' + str(sorted(existing - schema_inputs)))
                for p in schema_inputs: read(p)
                write(str(Path(dest) / 'gschemas.compiled'))
            elif name == 'xgettext':
                allowed = ('--package-name=', '--from-code=', '--add-comments=', '--directory=', '--output=', '--language=', '--keyword=', '--flag=')
                flags = {'--sort-by-file', '--join-existing'}
                if any(x.startswith('-') and not x.startswith(allowed) and x not in flags for x in a): raise ProducerError('unsupported xgettext option')
                directory = [x.split('=', 1)[1] for x in a if x.startswith('--directory=')]
                dest = [x.split('=', 1)[1] for x in a if x.startswith('--output=')]
                if len(directory) != 1 or len(dest) != 1: raise ProducerError('ambiguous xgettext directory/output')
                destination = patharg(cwd, dest[0])
                if '--join-existing' in a and destination not in writes: raise ProducerError('xgettext joins an unproduced output')
                for value in a:
                    if not value.startswith('-'): read(value, patharg(cwd, directory[0]))
                write(dest[0])
            elif name == 'msgfmt':
                if any(env.get(k) for k in ('GETTEXTDATADIRS', 'GETTEXTDATADIR', 'XGETTEXT_ITS_PATH')): raise ProducerError('gettext data path override is not admitted')
                bind_tree(Path(argv[0]).parent.parent / 'share/gettext/its', optional=True)
                if len(a) == 3 and a[0] == '--check' and a[1].startswith('--output-file='):
                    read(a[2]); write(a[1].split('=', 1)[1])
                elif len(a) == 6 and a[0] in {'--desktop', '--xml'} and a[1].startswith('--template=') and a[2] == '-d' and a[4] == '-o':
                    read(a[1].split('=', 1)[1]); directory = Path(patharg(cwd, a[3])); linguas = read(str(directory / 'LINGUAS'))
                    langs = Path(linguas).read_text().split()
                    expected = {str(directory / (lang + '.po')) for lang in langs}
                    existing = {str(p) for p in directory.glob('*.po')}
                    if existing - expected: raise ProducerError('locale directory has undeclared PO files')
                    for p in expected: read(p)
                    write(a[5])
                else: raise ProducerError('unsupported msgfmt argv')
            elif name.startswith('python'):
                if len(a) != 4 or Path(a[0]) != root / 'cmake/nostrc-pseudo-po.py' or a[3] != 'en@pseudo': raise ProducerError('unsupported custom Python producer')
                if any(env.get(k) for k in ('PYTHONHOME', 'PYTHONPATH')): raise ProducerError('custom Python import path override')
                read(a[0]); read(a[1]); write(a[2])
            else:
                raise ProducerError('unknown custom generator tool: ' + name)
        if not declared <= writes: raise ProducerError('native output not written by the admitted argv: ' + str(sorted(declared - writes)))
        if reads - native_inputs - writes - set(literals): raise ProducerError('generator reads undeclared inputs: ' + str(sorted(reads - native_inputs - writes - set(literals))))
        return sorted(reads), sorted(writes)

    done = set()
    for name, edge in snapshot['edges'].items():
        if edge['rule'] != 'CUSTOM_COMMAND': continue
        path = absolute(build, name)
        if path in done: continue
        try:
            if path not in native: raise ProducerError('native custom command has no literal command/output declaration')
            group, command = native[path]; done.update(group)
            matches = [d for d in custom if path in d['outputs']]
            if len(matches) != 1: raise ProducerError('missing/ambiguous source add_custom_command for ' + name)
            d = matches[0]
            if d.get('error'): raise ProducerError(d['error'])
            if set(d['outputs']) != group: raise ProducerError('source/native output sets differ')
            cwd, commands = shell_commands(command)
            if cwd != d['working_directory'] or commands != d['commands']: raise ProducerError('source/native generator argv or working directory differs')
            native_inputs = {absolute(build, value) for kind, value in edge['inputs'] if kind != 'order_only'}
            declared_inputs = set()
            for value in d['depends']:
                if any(t['name'] == value for t in targets.values()): continue
                p = absolute(d['binary'], value)
                if not Path(value).is_absolute() and p not in known and Path(absolute(d['source'], value)).exists(): p = absolute(d['source'], value)
                declared_inputs.add(p)
            if declared_inputs != native_inputs: raise ProducerError('source/native declared dependencies differ: ' + str(sorted(declared_inputs ^ native_inputs)))
            reads, writes = io_commands(commands, cwd, group, native_inputs)
            for p in native_inputs: file_input(p)
            outputs.update(writes)
            admitted.update(group)
            proofs[path] = dict(source=d, native_command=command, declared_inputs=sorted(native_inputs), reads=reads, outputs=writes)
        except (ProducerError, OSError, ValueError, ET.ParseError) as exc:
            gaps.append({'kind': 'unproven_default_custom_producer', 'output': name, 'reason': str(exc)})
    utility_ids, utility_proofs = [], {}
    def phony_leaves(name, seen=None):
        seen = set() if seen is None else seen
        if name in seen: raise ProducerError('cyclic utility phony closure')
        edge = snapshot['edges'].get(name)
        if not edge or edge['rule'] != 'phony': return {absolute(build, name)}
        return set().union(*(phony_leaves(value, seen | {name}) for kind, value in edge['inputs'] if kind != 'order_only'))
    for ident, target in targets.items():
        if target['type'] != 'UTILITY': continue
        scoped = str(Path(target['build_path']).relative_to(build) / target['name'])
        alias = target['name'] if target['name'] in snapshot['edges'] else scoped
        if alias not in snapshot['edges']: continue
        matches = utilities.get(target['name'], [])
        try:
            if len(matches) != 1 or matches[0].get('error') or matches[0]['commands'] or snapshot['edges'][alias]['rule'] != 'phony':
                raise ProducerError('utility is not a uniquely source-declared command-free phony aggregate')
            d = matches[0]
            if d['binary'] != target['build_path']: raise ProducerError('File API/source utility directory mismatch')
            expected = {absolute(d['binary'], p) for p in d['depends']}
            expected_targets = {name for dep in d['target_dependencies'] for name in dep['targets']}
            if expected_targets != {targets[key]['name'] for key in target['dependencies']}:
                raise ProducerError('source/File API utility target dependencies differ')
            for dependency in expected_targets:
                native_target = next(t for t in targets.values() if t['name'] == dependency)
                dependency_alias = dependency if dependency in snapshot['edges'] else str(Path(native_target['build_path']).relative_to(build) / dependency)
                if dependency_alias not in snapshot['edges']:
                    raise ProducerError('utility target dependency is absent from native default graph')
                expected.update(phony_leaves(dependency_alias))
            actual = phony_leaves(alias)
            # CMake repeats secondary outputs of a multi-output command on
            # utility edges, even when DEPENDS names only its primary output.
            pending = list(expected)
            while pending:
                path = pending.pop()
                if path in native:
                    group = native[path][0]
                    proof = next((proofs[p] for p in group if p in proofs), None)
                    expanded = group | ({p for p in proof['declared_inputs'] if p in native} if proof else set())
                    pending.extend(expanded - expected)
                    expected.update(expanded)
            if expected != actual: raise ProducerError('source/native utility prerequisite closure differs: ' + str(sorted(expected ^ actual)))
            utility_ids.append(ident); inputs.add(d['file']); inputs.update(dep['file'] for dep in d['target_dependencies'])
            utility_proofs[ident] = dict(source=d, native_alias=alias)
        except ProducerError as exc:
            gaps.append({'kind': 'unproven_default_utility', 'target': target['name'], 'reason': str(exc)})
    for p, proof in literals.items(): inputs.add(proof['file'])
    inputs.update(d['file'] for d in custom if any(p in admitted for p in d['outputs']))
    # All configured CMake inputs bind the context that expanded the declarations.
    inputs.update(realization['default_configure_context']['source_inputs'])
    return dict(producers=proofs, utilities=utility_proofs, admitted=sorted(admitted), outputs=sorted(outputs), literal_outputs=literals,
                utility_target_ids=sorted(utility_ids), inputs=sorted(inputs), tools=tools, gaps=gaps)
