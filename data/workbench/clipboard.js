import {ui, request} from 'astra/ui';

// Offsets and limits are UTF-8 bytes; JS string indexes are never sent as edits.
const limit = 8 * 1024 * 1024;
function byteLength(text) {
    let bytes = 0;
    for (const character of text) {
        const scalar = character.codePointAt(0);
        bytes += scalar < 0x80 ? 1 : scalar < 0x800 ? 2 : scalar < 0x10000 ? 3 : 4;
    }
    return bytes;
}
async function copy(cut) {
    const state = await ui.read(), document = state.document;
    if (!document) return;
    const ranges = document.selections.map((selection, index) => {
        const begin = Math.min(selection.anchor, selection.head), end = Math.max(selection.anchor, selection.head);
        if (begin !== end) return {begin, end};
        const line = state.selectionLines[index];
        if (!line) throw new Error('Wait for the current line to load before copying it');
        return {begin:line.begin, end:line.next};
    }).filter(range => range.begin !== range.end).sort((a,b) => a.begin - b.begin || a.end - b.end)
      .filter((range,index,ranges) => !index || range.begin !== ranges[index-1].begin || range.end !== ranges[index-1].end);
    if (!ranges.length) return;
    let text = '', bytes = 0;
    for (const [index, range] of ranges.entries()) {
        if (index && !text.endsWith('\n')) { text += '\n'; ++bytes; }
        let offset = range.begin;
        while (offset < range.end) {
            const part = await request('document.get', {id:document.id, begin:offset, end:range.end});
            if (part.revision !== document.revision) throw new Error('Document changed while copying; copy again');
            const next = part.range.end;
            if (next <= offset || next > range.end) throw new Error('Document copy made invalid progress');
            bytes += byteLength(part.text);
            if (bytes > limit) throw new Error('Selection exceeds the 8 MiB scripted clipboard limit');
            text += part.text; offset = next;
        }
    }
    await ui.clipboard.write(text);
    if (cut) {
        const metadata = await request('document.edit', {id:document.id, baseRevision:document.revision,
            edits:ranges.map(range => ({...range, text:''}))});
        await ui.update({document:metadata, revealCaret:true});
    }
    await ui.update({status:`${cut ? 'Cut' : 'Copied'} ${ranges.length} selection(s)`});
}
export function registerClipboard() {
    ui.registerCommand({id:'ui.copy',title:'Copy',shortcut:'Ctrl+C'}, () => copy(false));
    ui.registerCommand({id:'ui.cut',title:'Cut',shortcut:'Ctrl+X'}, () => copy(true));
    ui.registerCommand({id:'ui.paste',title:'Paste',shortcut:'Ctrl+V'}, async () => {
        const state = await ui.read(), document = state.document;
        if (!document) return;
        const input = await ui.clipboard.read();
        if (!input) return;
        if (byteLength(input) > limit) throw new Error('Text exceeds the 8 MiB scripted clipboard limit');
        const text = input.replace(/\r\n|\r|\n/g, state.newline);
        const metadata = await request('script.command', {name:'editor.insert_text',
            params:{id:document.id,baseRevision:document.revision,text}});
        await ui.update({document:metadata,revealCaret:true,status:'Pasted'});
    });
}
