import { ui, request } from 'astra/ui';

// Offsets and limits are UTF-8 bytes; JS string indexes are never sent as edits.
const limit = 8 * 1024 * 1024;
async function copy(cut) {
    const state = await ui.read(),
        document = state.document;
    if (!document) return;
    const lines = new Map();
    const ranges = [];
    for (const [index, selection] of document.selections.entries()) {
        const begin = Math.min(selection.anchor, selection.head),
            end = Math.max(selection.anchor, selection.head);
        if (begin !== end) {
            ranges.push({ begin, end });
            continue;
        }
        let line = state.selectionLines?.[index];
        if (!line) {
            const number = document.selectionPositions?.[index]?.line;
            if (!Number.isSafeInteger(number))
                throw new Error('The selected line has no document position');
            line = lines.get(number);
            if (!line) {
                const page = await request('document.lines', {
                    id: document.id,
                    firstLine: number,
                    count: 1
                });
                if (page.revision !== document.revision)
                    throw new Error('Document changed while copying; copy again');
                const source = page.lines[0];
                if (!source || source.line !== number)
                    throw new Error('Document copy returned the wrong line');
                line = { begin: source.begin, next: source.nextBegin };
                lines.set(number, line);
            }
        }
        ranges.push({ begin: line.begin, end: line.next });
    }
    const unique = ranges
        .filter((range) => range.begin !== range.end)
        .sort((a, b) => a.begin - b.begin || a.end - b.end)
        .filter(
            (range, index, ranges) =>
                !index ||
                range.begin !== ranges[index - 1].begin ||
                range.end !== ranges[index - 1].end
        );
    if (!unique.length) return;
    let text = '',
        bytes = 0;
    for (const [index, range] of unique.entries()) {
        if (index && !text.endsWith('\n')) {
            text += '\n';
            ++bytes;
        }
        let offset = range.begin;
        while (offset < range.end) {
            const part = await request('document.get', {
                id: document.id,
                begin: offset,
                end: range.end
            });
            if (part.revision !== document.revision)
                throw new Error('Document changed while copying; copy again');
            const next = part.range.end;
            if (next <= offset || next > range.end)
                throw new Error('Document copy made invalid progress');
            bytes += next - offset;
            if (bytes > limit)
                throw new Error('Selection exceeds the 8 MiB scripted clipboard limit');
            text += part.text;
            offset = next;
        }
    }
    await ui.clipboard.write(text);
    if (cut) {
        const metadata = await request('document.edit', {
            id: document.id,
            baseRevision: document.revision,
            edits: unique.map((range) => ({ ...range, text: '' }))
        });
        await ui.update({ document: metadata, revealCaret: true });
    }
    await ui.update({ status: `${cut ? 'Cut' : 'Copied'} ${unique.length} selection(s)` });
}
export function registerClipboard() {
    ui.registerCommand({ id: 'ui.copy', title: 'Copy', shortcut: 'Ctrl+C' }, () => copy(false));
    ui.registerCommand({ id: 'ui.cut', title: 'Cut', shortcut: 'Ctrl+X' }, () => copy(true));
    ui.registerCommand({ id: 'ui.paste', title: 'Paste', shortcut: 'Ctrl+V' }, async () => {
        const state = await ui.read(),
            document = state.document;
        if (!document) return;
        const input = await ui.clipboard.read();
        if (!input) return;
        if (input.length > limit || astra.utf8ByteLength(input) > limit)
            throw new Error('Text exceeds the 8 MiB scripted clipboard limit');
        const text = input.replace(/\r\n|\r|\n/g, state.newline);
        const metadata = await request('script.command', {
            name: 'editor.insert_text',
            params: { id: document.id, baseRevision: document.revision, text }
        });
        await ui.update({ document: metadata, revealCaret: true, status: 'Pasted' });
    });
}
