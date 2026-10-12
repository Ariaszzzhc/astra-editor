import { ui, request } from 'astra/ui';
let interactive;
let hoverControl;
async function languageResult(method, present, parameters = {}) {
    interactive?.abort();
    const controller = new AbortController();
    interactive = controller;
    const options = { signal: controller.signal };
    try {
        const { document } = await ui.read();
        if (!document) return;
        if (
            (parameters.id !== undefined && parameters.id !== document.id) ||
            (parameters.baseRevision !== undefined && parameters.baseRevision !== document.revision)
        )
            return;
        await ui.update({ languageActive: { id: document.id, active: true } });
        const offset = parameters.byteOffset ?? document.selections[0].head;
        const result = await request(
            'language.request',
            { id: document.id, baseRevision: document.revision, method, byteOffset: offset },
            options
        );
        const current = (await ui.read()).document;
        if (
            result.obsolete ||
            current?.id !== document.id ||
            current.revision !== result.revision
        ) {
            await ui.update({ status: 'Discarded a language result for an older revision' });
            return;
        }
        await present(result, document, offset, options);
    } catch (error) {
        if (error.name !== 'AbortError') throw error;
    } finally {
        if (interactive === controller) interactive = undefined;
    }
}
function tooltipText(value) {
    let text = String(value).slice(0, 16384);
    if (/[\ud800-\udbff]$/.test(text)) text = text.slice(0, -1);
    while (astra.utf8ByteLength(text) > 16384) {
        text = text.slice(0, Math.floor(text.length * 0.75));
        if (/[\ud800-\udbff]$/.test(text)) text = text.slice(0, -1);
    }
    return text.replaceAll('\0', '');
}
function hoverText(contents) {
    let text = '';
    const parts = Array.isArray(contents)
        ? contents.slice(0, 128)
        : contents == null
          ? []
          : [contents];
    for (const part of parts) {
        const value = typeof part === 'string' ? part : part.value || '';
        if (text) text += '\n';
        text += value.slice(0, 16384 - text.length);
        if (text.length >= 16384) break;
    }
    return tooltipText(text)
        .replace(/^```[^\n]*\n?/gm, '')
        .replace(/^#{1,6} /gm, '')
        .replace(/\*\*([^*\n]+)\*\*/g, '$1');
}
async function editorHover({ version, target }) {
    hoverControl?.abort();
    hoverControl = undefined;
    if (!target) return;
    const control = new AbortController();
    hoverControl = control;
    const options = { signal: control.signal, timeoutMs: 5000 };
    let settled = false;
    try {
        await astra.sleep(350, options);
        const { document } = await ui.read({ immediate: true }, options);
        if (document?.id !== target.id || document.revision !== target.baseRevision) return;
        const diagnostics = await request('language.diagnostics', { id: target.id }, options);
        const messages = [];
        if (diagnostics.revision === target.baseRevision) {
            let position = 0;
            for (const item of diagnostics.diagnostics) {
                const start = item.range.start,
                    end = item.range.end;
                const after =
                    target.line > start.line ||
                    (target.line === start.line && target.utf16Column >= start.character);
                const before =
                    target.line < end.line ||
                    (target.line === end.line && target.utf16Column < end.character);
                const empty =
                    start.line === end.line &&
                    start.character === end.character &&
                    target.line === start.line &&
                    target.utf16Column === start.character;
                if (after && (before || empty)) messages.push(tooltipText(item.message));
                if (messages.length === 8) break;
                if (++position % 128 === 0) await astra.sleep(0, options);
            }
        }
        if (diagnostics.available) {
            const result = await request(
                'language.request',
                {
                    id: target.id,
                    baseRevision: target.baseRevision,
                    method: 'hover',
                    byteOffset: target.byteOffset
                },
                options
            );
            if (result.obsolete || result.revision !== target.baseRevision) return;
            const text = hoverText(result.result?.contents);
            if (text) messages.push(text);
        } else if (diagnostics.error) messages.push(diagnostics.reason + ': ' + diagnostics.error);
        await ui.editorTooltip.publish(
            { version, text: tooltipText(messages.join('\n\n')) },
            options
        );
        settled = true;
    } catch (error) {
        if (error.name !== 'AbortError') {
            await ui.editorTooltip.publish({
                version,
                text: tooltipText(`Language information unavailable: ${error.message}`)
            });
            settled = true;
        }
    } finally {
        if (!settled && !control.signal.aborted)
            await ui.editorTooltip.publish({ version, text: '' });
        if (hoverControl === control) hoverControl = undefined;
    }
}
async function navigate(item, options) {
    if (!item.path) {
        await ui.update({ status: `Location is outside the workspace: ${item.uri || ''}` });
        return;
    }
    let document = await request('workspace.open', { path: item.path }, options);
    if (item.targetRevision !== undefined && item.targetRevision !== document.revision) {
        await ui.update({ status: 'Navigation target changed; request the location again' });
        return;
    }
    if (item.byteOffset !== undefined) {
        document = await request(
            'document.setSelections',
            {
                id: document.id,
                baseRevision: document.revision,
                selections: [
                    { anchor: item.byteOffset, head: item.endByteOffset ?? item.byteOffset }
                ]
            },
            options
        );
        await ui.update({
            document,
            activate: document.id,
            revealCaret: true,
            status: 'Location opened'
        });
    } else
        await ui.update({
            document,
            activate: document.id,
            scrollLine: { id: document.id, line: item.line || 0 },
            status: 'Location opened'
        });
}
export function registerLanguage() {
    astra.on('ui.editor.hover', editorHover);
    for (const [id, title, kind, inner] of [
        ['ui.selectFunction', 'Select enclosing function', 'function', false],
        ['ui.selectClass', 'Select enclosing class', 'class', false],
        ['ui.selectBlock', 'Select enclosing block', 'block', false],
        ['ui.selectString', 'Select string content', 'string', true]
    ]) {
        ui.registerCommand({ id, title }, (parameters) =>
            ui.selectObject(
                kind,
                { ...parameters, inner: parameters.inner ?? inner },
                { signal: parameters.signal, timeoutMs: parameters.timeoutMs }
            )
        );
    }
    ui.registerCommand(
        { id: 'ui.completion', title: 'Trigger completion', shortcut: 'Ctrl+Space' },
        () =>
            languageResult('completion', async (result, document, offset, options) => {
                const items = Array.isArray(result.result)
                    ? result.result
                    : result.result?.items || [];
                if (!items.length) {
                    await ui.update({ status: 'No completion items' });
                    return;
                }
                const choice = await ui.pick(
                    {
                        title: 'COMPLETION — TYPE TO FILTER · ENTER TO INSERT',
                        items: items.map((item) => ({ label: item.label, item }))
                    },
                    options
                );
                if (!choice) return;
                const current = (await ui.read()).document;
                if (
                    current?.id !== document.id ||
                    current.revision !== result.revision ||
                    current.selections[0].head !== offset
                ) {
                    await ui.update({ status: 'Completion became stale; request it again' });
                    return;
                }
                const metadata = await request(
                    'language.applyCompletion',
                    {
                        id: document.id,
                        baseRevision: result.revision,
                        byteOffset: offset,
                        completionItem: choice.item
                    },
                    options
                );
                await ui.update({
                    document: metadata,
                    revealCaret: true,
                    status: 'Completion applied'
                });
            })
    );
    ui.registerCommand(
        { id: 'ui.hover', title: 'Show hover information', shortcut: 'Ctrl+K' },
        () =>
            languageResult('hover', async (result, _, __, options) => {
                const contents = result.result?.contents;
                const parts = Array.isArray(contents)
                    ? contents
                    : contents == null
                      ? []
                      : [contents];
                const text = parts
                    .map((part) => (typeof part === 'string' ? part : part.value || ''))
                    .join('\n');
                if (!text) {
                    await ui.update({ status: 'No hover information' });
                    return;
                }
                await ui.showText(
                    { title: 'LANGUAGE INFORMATION — Esc to close · arrows to scroll', text },
                    options
                );
            })
    );
    for (const [method, shortcut, title] of [
        ['definition', 'F12', 'Go to definition'],
        ['references', 'Shift+F12', 'Find all references']
    ]) {
        ui.registerCommand({ id: `ui.${method}`, title, shortcut }, (parameters) =>
            languageResult(
                method,
                async (result, _, __, options) => {
                    const locations = result.resolvedLocations || [];
                    if (!locations.length) {
                        await ui.update({ status: 'No locations found' });
                        return;
                    }
                    const choice =
                        locations.length === 1
                            ? { item: locations[0] }
                            : await ui.pick(
                                  {
                                      title: 'LANGUAGE LOCATIONS — ENTER TO OPEN',
                                      items: locations.map((item) => ({
                                          label: `${item.path || item.uri}:${(item.line || 0) + 1}`,
                                          item
                                      }))
                                  },
                                  options
                              );
                    if (choice) await navigate(choice.item, options);
                },
                parameters
            )
        );
    }
    ui.registerCommand(
        { id: 'ui.diagnostics', title: 'Show document diagnostics', shortcut: 'Ctrl+Shift+M' },
        async () => {
            const { document } = await ui.read();
            if (!document) return;
            await ui.update({ languageActive: { id: document.id, active: true } });
            const result = await request('language.diagnostics', { id: document.id });
            if (result.revision !== document.revision) {
                await ui.update({ status: 'Diagnostics changed; request them again' });
                return;
            }
            const text =
                result.diagnostics
                    .map(
                        (item) =>
                            `${item.range.start.line + 1}:${item.range.start.character + 1}  ${item.message}`
                    )
                    .join('\n') ||
                (result.pending
                    ? 'Waiting for language diagnostics...'
                    : result.reason || 'No diagnostics');
            await ui.showText({
                title: 'DOCUMENT DIAGNOSTICS — Esc to close · arrows to scroll',
                text
            });
        }
    );
    ui.registerCommand(
        { id: 'ui.format', title: 'Format document', shortcut: 'Shift+Alt+F' },
        async () => {
            const { document } = await ui.read();
            if (!document) return;
            const result = await request('language.request', {
                id: document.id,
                baseRevision: document.revision,
                method: 'formatting'
            });
            if (result.obsolete) {
                await ui.update({ status: 'Discarded a language result for an older revision' });
                return;
            }
            if (!result.result?.length) {
                await ui.update({ status: 'Document is already formatted' });
                return;
            }
            const metadata = await request('language.applyTextEdits', {
                id: document.id,
                baseRevision: result.revision,
                edits: result.result
            });
            await ui.update({
                document: metadata,
                revealCaret: true,
                status: 'Document formatted'
            });
        }
    );
    ui.registerCommand({ id: 'ui.rename', title: 'Rename symbol', shortcut: 'F2' }, async () => {
        const { document } = await ui.read();
        if (!document) return;
        const newName = await ui.input({ title: 'RENAME SYMBOL — NEW NAME' });
        if (!newName) return;
        const result = await request('language.request', {
            id: document.id,
            baseRevision: document.revision,
            method: 'rename',
            byteOffset: document.selections[0].head,
            newName
        });
        if (result.obsolete) {
            await ui.update({ status: 'Discarded a language result for an older revision' });
            return;
        }
        if (!result.planId || !result.documents?.length) {
            await ui.update({ status: 'No rename edits' });
            return;
        }
        const accepted = await ui.confirm({
            title: 'Apply symbol rename?',
            accept: 'Apply rename',
            detail: `${result.documents.length} file(s) will become edited buffers. Save afterward to persist changes.`,
            items: result.documents.map((file) => `${file.path} (${file.editCount} edits)`)
        });
        if (!accepted) return;
        const applied = await request('language.applyWorkspaceEdit', { planId: result.planId });
        await ui.update({
            documents: applied.documents,
            status: 'Rename applied to buffers; save files to persist'
        });
    });
}
