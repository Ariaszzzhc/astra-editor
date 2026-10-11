import { ui, request } from 'astra/ui';

export function registerOutline() {
    ui.registerOutline({ loading: 'Parsing document...' }, symbols, { activate: selectSymbol });
}

async function symbols(viewport, options) {
    const parameters = {
        id: viewport.id,
        baseRevision: viewport.revision,
        begin: 0,
        end: 0,
        includeOutline: true,
        outlineBegin: viewport.offset,
        outlineLimit: viewport.limit
    };
    let syntax = await request('document.syntax', parameters, options);
    // An asynchronous parse completes with its identity. Read the actual indexed
    // page after that completion, without copying all symbols into JavaScript.
    if (syntax.state === 'ready' && !syntax.outlinePage)
        syntax = await request('document.syntax', parameters, options);
    if (
        syntax.id !== viewport.id ||
        syntax.revision !== viewport.revision ||
        syntax.state === 'obsolete'
    )
        return { offset: 0, total: 0, rows: [], empty: 'Document changed; refresh Outline' };
    if (syntax.state === 'unsupported')
        return { offset: 0, total: 0, rows: [], empty: 'No parser for this file type' };
    if (syntax.state !== 'ready' || !syntax.outlinePage || !Array.isArray(syntax.outline))
        throw new Error('Current document symbols could not be prepared');
    if (viewport.offset >= syntax.outlinePage.total && syntax.outlinePage.total) {
        parameters.outlineBegin = Math.max(0, syntax.outlinePage.total - viewport.limit + 1);
        syntax = await request('document.syntax', parameters, options);
        if (
            syntax.state !== 'ready' ||
            !syntax.outlinePage ||
            syntax.revision !== viewport.revision
        )
            throw new Error('Document changed while seeking its symbol page');
    }
    return {
        offset: syntax.outlinePage.offset,
        total: syntax.outlinePage.total,
        empty: 'No symbols',
        rows: syntax.outline.map((symbol, index) => ({
            id: `symbol-${syntax.outlinePage.offset + index}`,
            label: symbol.name,
            depth: Math.min(symbol.depth ?? 0, 8),
            icon: symbol.kind === 'function' ? 'function' : 'symbol',
            iconTone: 'accent',
            data: { selectionBegin: symbol.selectionBegin, selectionEnd: symbol.selectionEnd }
        }))
    };
}

async function selectSymbol(symbol, row, identity) {
    const state = await ui.read({ id: identity.id });
    const document = state.document;
    if (
        !document ||
        state.activity !== identity.activity ||
        document.id !== identity.id ||
        document.revision !== identity.revision
    )
        return null;
    const selected = await request('document.setSelections', {
        id: identity.id,
        baseRevision: identity.revision,
        selections: [{ anchor: symbol.selectionBegin, head: symbol.selectionEnd }]
    });
    await ui.update({ document: selected, revealCaretFor: identity.id });
    return selected;
}
