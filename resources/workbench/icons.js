// File associations are client policy. Native code receives drawing descriptors
// and never interprets extensions, filenames or directory names.
const request = (method, params = {}, options) => astra.request(method, params, options);
const glyphs = new Set([
    'files',
    'search',
    'branch',
    'settings',
    'folder',
    'document',
    'code',
    'braces',
    'symbol',
    'function',
    'refresh',
    'warning'
]);
const tones = new Set([
    'muted',
    'foreground',
    'folder',
    'codeIcon',
    'scriptIcon',
    'accent',
    'added',
    'changed',
    'error',
    'warning'
]);
let compiled;
function object(value) {
    return value !== null && typeof value === 'object' && !Array.isArray(value);
}
function bytes(value) {
    // Native regular expressions count UTF-8 expansion without running one JS
    // callback per ASCII character in long workspace paths.
    return (
        value.length +
        (value.match(/[\u0080-\u07ff]/g)?.length ?? 0) +
        2 * (value.match(/[\u0800-\ud7ff\ue000-\uffff]/g)?.length ?? 0) +
        2 * (value.match(/[\u{10000}-\u{10ffff}]/gu)?.length ?? 0)
    );
}
function capture(value, depth = 0) {
    if (
        value === null ||
        typeof value === 'string' ||
        typeof value === 'boolean' ||
        (typeof value === 'number' && Number.isFinite(value))
    )
        return value;
    if (
        !object(value) ||
        depth > 4 ||
        Object.keys(value).length > 256 ||
        ![null, Object.prototype].includes(Object.getPrototypeOf(value))
    ) {
        throw new TypeError('File icon patches require bounded JSON objects');
    }
    const result = Object.create(null);
    for (const [key, item] of Object.entries(value)) result[key] = capture(item, depth + 1);
    return result;
}
function descriptor(value) {
    if (
        !object(value) ||
        Object.keys(value).some((key) => key !== 'glyph' && key !== 'tone') ||
        !glyphs.has(value.glyph) ||
        !(tones.has(value.tone) || /^#[0-9a-fA-F]{6}$/.test(value.tone))
    ) {
        throw new TypeError('Invalid file icon drawing descriptor');
    }
    return Object.freeze({ glyph: value.glyph, tone: value.tone });
}
function table(value, extensions = false) {
    if (!object(value) || Object.keys(value).length > 256) {
        throw new TypeError('File icon association tables require at most 256 entries');
    }
    const result = new Map();
    for (const [key, icon] of Object.entries(value)) {
        if (
            !key ||
            bytes(key) > 128 ||
            /[\\/\u0000-\u001f\ud800-\udfff]/u.test(key) ||
            (extensions && (!key.startsWith('.') || key.length === 1))
        ) {
            throw new TypeError('Invalid file icon association key');
        }
        const normalized = key.toLowerCase();
        if (result.has(normalized)) throw new TypeError('Duplicate file icon association');
        result.set(normalized, descriptor(icon));
    }
    return result;
}
function compile(theme) {
    const fields = ['fallback', 'folder', 'filenames', 'extensions', 'directories'];
    if (
        !object(theme) ||
        Object.keys(theme).some((key) => !fields.includes(key)) ||
        JSON.stringify(theme).length > 65536
    ) {
        throw new TypeError('Invalid or oversized file icon theme');
    }
    return {
        fallback: descriptor(theme.fallback),
        folder: descriptor(theme.folder),
        filenames: table(theme.filenames),
        extensions: table(theme.extensions, true),
        directories: table(theme.directories)
    };
}
function captureItems(items, limit) {
    if (!Array.isArray(items) || items.length > limit) {
        throw new TypeError(`File icon resolution admits at most ${limit} paths`);
    }
    let total = 0;
    return items.map((item) => {
        if (
            !object(item) ||
            Object.keys(item).some((key) => key !== 'path' && key !== 'directory') ||
            typeof item.path !== 'string' ||
            item.path.length > 32768 ||
            typeof item.directory !== 'boolean'
        ) {
            throw new TypeError('Invalid file icon path');
        }
        total += item.path.length;
        if (total > (limit === 32 ? 65536 : 262144)) {
            throw new TypeError('File icon paths exceed their batch byte bound');
        }
        return { path: item.path, directory: item.directory };
    });
}
async function inputs(items, limit, options) {
    let total = 0,
        units = 0;
    for (const item of items) {
        let pathBytes = 0;
        for (let offset = 0; offset < item.path.length; ) {
            let end = Math.min(offset + 1024, item.path.length);
            const last = item.path.charCodeAt(end - 1);
            if (end < item.path.length && last >= 0xd800 && last <= 0xdbff) --end;
            const part = item.path.slice(offset, end);
            if (/[\u0000-\u001f\ud800-\udfff]/u.test(part)) {
                throw new TypeError('Invalid file icon path');
            }
            const size = bytes(part);
            pathBytes += size;
            total += size;
            if (pathBytes > 32768 || total > (limit === 32 ? 65536 : 262144)) {
                throw new TypeError('File icon paths exceed their byte bound');
            }
            units += end - offset;
            offset = end;
            if (units >= 4096) {
                options?.signal?.throwIfAborted();
                await astra.sleep(0);
                units = 0;
            }
        }
    }
    options?.signal?.throwIfAborted();
}
function classify(theme, item) {
    const slash = Math.max(item.path.lastIndexOf('/'), item.path.lastIndexOf('\\'));
    const name = item.path.slice(slash + 1).toLowerCase();
    if (item.directory) return theme.directories.get(name) ?? theme.folder;
    const exact = theme.filenames.get(name);
    if (exact) return exact;
    // Rule keys are at most 128 UTF-8 bytes. Only inspect that bounded suffix;
    // retain the longest match without allocating every suffix of a long name.
    let selected = theme.fallback;
    for (
        let dot = name.lastIndexOf('.');
        dot >= 0 && name.length - dot <= 128;
        dot = name.lastIndexOf('.', dot - 1)
    ) {
        const match = theme.extensions.get(name.slice(dot));
        if (match) selected = match;
        if (dot === 0) break;
    }
    return selected;
}
async function snapshot(options) {
    const state = await request('_ui.icons.read', {}, options);
    if (!compiled || compiled.generation !== state.generation) {
        compiled = { generation: state.generation, theme: compile(state.theme) };
    }
    return { ...state, compiled: compiled.theme };
}
function merge(target, patch) {
    if (!object(patch)) return patch;
    const result = Object.assign(Object.create(null), object(target) ? target : {});
    for (const [key, value] of Object.entries(patch)) {
        if (value === null) delete result[key];
        else result[key] = merge(result[key], value);
    }
    return result;
}
async function change(value, options, replace) {
    if (!object(value)) throw new TypeError('File icon updates require an object');
    const captured = capture(value);
    if (JSON.stringify(captured).length > 65536) {
        throw new TypeError('File icon update exceeds its size bound');
    }
    const state = await request('_ui.icons.read', {}, options);
    const theme = replace ? captured : merge(state.theme, captured);
    compile(theme);
    options?.signal?.throwIfAborted();
    const result = await request(
        '_ui.icons.update',
        {
            generation: state.generation,
            theme
        },
        options
    );
    compiled = undefined;
    return result;
}
export const fileIcons = {
    async read(options) {
        const { theme, generation } = await snapshot(options);
        return { theme, generation };
    },
    async resolve(items, options) {
        // Capture caller values before the first await.
        const captured = captureItems(items, 256);
        await inputs(captured, 256, options);
        const state = await snapshot(options);
        return {
            generation: state.generation,
            items: captured.map((item) => ({
                ...item,
                icon: classify(state.compiled, item)
            }))
        };
    },
    async update(patch, options) {
        return await change(patch, options, false);
    },
    async replace(theme, options) {
        return await change(theme, options, true);
    }
};
export function registerIcons() {
    astra.registerCommand('workbench.resolveIcons', async ({ generation, items }) => {
        const captured = captureItems(items, 32);
        await inputs(captured, 32);
        const state = await snapshot({ timeoutMs: 30000 });
        if (generation !== state.generation) return { generation, items: null };
        return {
            generation,
            items: captured.map((item) => ({
                ...item,
                icon: classify(state.compiled, item)
            }))
        };
    });
}
