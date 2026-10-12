"""Verify vendored SVGs and emit immutable bytes into the build directory."""
import argparse
import hashlib
import json
from pathlib import Path
import re
import xml.etree.ElementTree as ET

ET.register_namespace('', 'http://www.w3.org/2000/svg')


def prepare(root, output):
    manifest = json.loads((root / 'dependencies/icons/manifest.json').read_text('utf-8'))
    assets = manifest['assets']
    if not 15 <= len(assets) <= 4096:
        raise ValueError('Icon count outside the reviewed bound')
    ids = [item['id'] for item in assets]
    if ids != sorted(set(ids)):
        raise ValueError('Icon IDs must be unique and sorted')
    verified = {}
    for item in [*assets, *manifest['notices'], manifest['associations']]:
        relative = Path(item['path'])
        if relative.is_absolute() or '..' in relative.parts:
            raise ValueError('Resource path escapes repository')
        data = (root / relative).read_bytes()
        if hashlib.sha256(data).hexdigest() != item['sha256']:
            raise ValueError(f'Icon digest mismatch: {relative}')
        verified[item['path']] = data
    actual = {p.relative_to(root).as_posix() for p in (root / 'resources/icons').rglob('*.svg')}
    if actual != {item['path'] for item in assets}:
        raise ValueError('SVG membership differs from the icon inventory')
    policy = json.loads(verified[manifest['associations']['path']])
    if policy['assets'] != ids:
        raise ValueError('JS and native icon catalogs differ')
    descriptors = [policy['fallback'], policy['folder']]
    for table in ('filenames', 'extensions', 'directories'):
        descriptors.extend(policy[table].values())
    for descriptor in descriptors:
        if not descriptor.get('asset') or any(value not in ids for value in descriptor.values()):
            raise ValueError('Association references an unshipped resource')
    source = ['// Generated from verified upstream SVGs. Do not edit.',
              '#include <astra/ui/icons.hpp>', 'namespace astra::ui {', 'namespace {',
              'constexpr IconResource resources[] = {']
    total = 0
    for item in assets:
        data = verified[item['path']]
        if len(data) > 128 * 1024 or b'<!DOCTYPE' in data.upper() or b'<!ENTITY' in data.upper():
            raise ValueError(f'Unsupported SVG input: {item["id"]}')
        svg = ET.fromstring(data)
        if svg.tag != '{http://www.w3.org/2000/svg}svg' or 'viewBox' not in svg.attrib:
            raise ValueError(f'SVG requires root and viewBox: {item["id"]}')
        for node in svg.iter():
            tag = node.tag.split('}')[-1]
            if tag in {'script', 'image', 'text', 'foreignObject', 'animate', 'set'}:
                raise ValueError(f'Unsupported icon element: {item["id"]}/{tag}')
            for key, value in node.attrib.items():
                if (key.split('}')[-1] == 'href' and not value.startswith('#')) or any(
                        not ref.strip().strip('\"\'').startswith('#')
                        for ref in re.findall(r'url\((.*?)\)', value)):
                    raise ValueError(f'External SVG resource: {item["id"]}')
        # Keep checked-in SVGs byte-for-byte upstream. Only embedded build data
        # uses a common viewport and explicit currentColor for tinting.
        svg.set('width', '24')
        svg.set('height', '24')
        if item['monochrome']:
            svg.set('color', '#ffffff')
        embedded = ET.tostring(svg, encoding='unicode')
        if ')ASTRA_SVG"' in embedded:
            raise ValueError('Raw string delimiter in SVG')
        source.append('{"' + item['id'] + '", R"ASTRA_SVG(' + embedded +
                      ')ASTRA_SVG", ' + str(item['monochrome']).lower() + '},')
        total += len(data)
    source += ['};', '} // namespace',
               'std::span<const IconResource> icon_resources() { return resources; }',
               '} // namespace astra::ui', '']
    output.mkdir(parents=True, exist_ok=True)
    destination = output / 'icon_data.cpp'
    generated = '\n'.join(source)
    if not destination.exists() or destination.read_text('utf-8') != generated:
        destination.write_text(generated, encoding='utf-8', newline='\n')
    print(f'Verified {len(assets)} icons, {total} source bytes and all notices/associations')


if __name__ == '__main__':
    parser = argparse.ArgumentParser()
    parser.add_argument('--root', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    prepare(args.root.resolve(), args.output.resolve())
