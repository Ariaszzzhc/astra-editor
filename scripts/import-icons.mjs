// Maintenance only: normal builds use local, hash-verified upstream assets.
import fs from 'node:fs';
import path from 'node:path';
import crypto from 'node:crypto';

const root = path.resolve(import.meta.dirname, '..');
const manifestPath = path.join(root, 'dependencies/icons/manifest.json');
const manifest = JSON.parse(fs.readFileSync(manifestPath, 'utf8'));
const cache = path.resolve(process.argv[2] ?? path.join(root, '.tools/icons'));
const hash = (data) => crypto.createHash('sha256').update(data).digest('hex');
const upstream = {};
for (const [name, source] of Object.entries(manifest.sources)) {
    const archive = path.join(cache, source.cacheFile);
    if (hash(fs.readFileSync(archive)) !== source.sha256)
        throw new Error(`Archive digest: ${name}`);
    upstream[name] = path.join(cache, source.cacheDirectory);
}
const theme = JSON.parse(fs.readFileSync(path.join(upstream.material, 'dist/material-icons.json')));
const inventory = [];
const copied = new Set();
function copyAsset(pack, name) {
    const id = `${pack}/${name}`;
    if (copied.has(id)) return id;
    const source =
        pack === 'codicons'
            ? `src/icons/${name}.svg`
            : path.posix.normalize('dist/' + theme.iconDefinitions[name].iconPath);
    if (pack === 'material' && (!source.startsWith('icons/') || source.includes('..'))) {
        throw new Error(`Unexpected upstream icon path: ${source}`);
    }
    const data = fs.readFileSync(path.join(upstream[pack], source));
    const destination = `resources/icons/${pack}/${name}.svg`;
    fs.mkdirSync(path.dirname(path.join(root, destination)), { recursive: true });
    fs.writeFileSync(path.join(root, destination), data);
    inventory.push({
        id,
        path: destination,
        source,
        sha256: hash(data),
        monochrome: pack === 'codicons'
    });
    copied.add(id);
    return id;
}
const ui = {
    files: 'files',
    search: 'search',
    branch: 'git-branch',
    settings: 'settings-gear',
    close: 'close',
    chevron_right: 'chevron-right',
    chevron_down: 'chevron-down',
    folder: 'folder',
    document: 'file',
    code: 'code',
    braces: 'json',
    symbol: 'symbol-misc',
    function: 'symbol-method',
    refresh: 'refresh',
    warning: 'warning'
};
for (const name of Object.values(ui)) copyAsset('codicons', name);
function descriptor(name, lightName, openName, openLightName) {
    const result = { asset: copyAsset('material', name) };
    if (lightName && lightName !== name) result.lightAsset = copyAsset('material', lightName);
    if (openName) result.openAsset = copyAsset('material', openName);
    if (openLightName && openLightName !== openName)
        result.openLightAsset = copyAsset('material', openLightName);
    return result;
}
const associations = {
    assets: [],
    fallback: descriptor(theme.file, theme.light?.file),
    folder: descriptor(
        theme.folder,
        theme.light?.folder,
        theme.folderExpanded,
        theme.light?.folderExpanded
    ),
    filenames: {},
    extensions: {},
    directories: {}
};
for (const [target, table, expanded] of [
    ['filenames', 'fileNames'],
    ['extensions', 'fileExtensions'],
    ['directories', 'folderNames', 'folderNamesExpanded']
]) {
    for (let [key, name] of Object.entries(theme[table])) {
        // Preserve Astra's basename association semantics.
        if (key.includes('/') || key.includes('\\')) continue;
        const value = descriptor(
            name,
            theme.light?.[table]?.[key],
            expanded ? theme[expanded]?.[key] : undefined,
            expanded ? theme.light?.[expanded]?.[key] : undefined
        );
        if (target === 'extensions') key = '.' + key;
        associations[target][key.toLowerCase()] = value;
    }
}
inventory.sort((a, b) => (a.id < b.id ? -1 : a.id > b.id ? 1 : 0));
associations.assets = inventory.map((asset) => asset.id);
const policyPath = 'resources/icons/associations.json';
fs.writeFileSync(path.join(root, policyPath), JSON.stringify(associations, null, 2) + '\n');
const notices = [];
for (const [pack, filenames] of Object.entries({
    codicons: ['LICENSE', 'LICENSE-CODE'],
    material: ['LICENSE']
})) {
    for (const name of filenames) {
        const data = fs.readFileSync(path.join(upstream[pack], name));
        const destination = `dependencies/icons/licenses/${pack}/${name}`;
        fs.mkdirSync(path.dirname(path.join(root, destination)), { recursive: true });
        fs.writeFileSync(path.join(root, destination), data);
        notices.push({ path: destination, source: pack, sha256: hash(data) });
    }
}
manifest.ui = ui;
manifest.assets = inventory;
manifest.notices = notices;
manifest.associations = {
    path: policyPath,
    sha256: hash(fs.readFileSync(path.join(root, policyPath)))
};
fs.writeFileSync(manifestPath, JSON.stringify(manifest, null, 2) + '\n');
console.log(
    `Imported ${inventory.length} SVGs; ${Object.keys(associations.filenames).length} filenames, ` +
        `${Object.keys(associations.extensions).length} extensions, ${Object.keys(associations.directories).length} folders.`
);
