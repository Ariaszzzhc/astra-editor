import { ui, request } from 'astra/ui';
export function registerFiles() {
    ui.registerCommand(
        { id: 'ui.quickOpen', title: 'Open file by path', shortcut: 'Ctrl+P' },
        async () => {
            const path = await ui.input({ title: 'OPEN WORKSPACE FILE' });
            if (!path) return;
            const document = await request('workspace.open', { path });
            await ui.update({ document, activate: document.id, status: 'File opened' });
        }
    );
    ui.registerCommand(
        { id: 'ui.saveAs', title: 'Save file as...', shortcut: 'Ctrl+Shift+S' },
        (parameters) => save(true, parameters)
    );
    ui.registerCommand(
        { id: 'ui.closeTab', title: 'Close active tab', shortcut: 'Ctrl+W' },
        (parameters) => close(parameters)
    );
    ui.registerCommand({ id: 'workbench.close', title: 'Close captured document' }, (parameters) =>
        close(parameters)
    );
    for (const [id, index] of [
        ['ui.saveAndClose', 0],
        ['ui.discard', 1]
    ])
        ui.registerCommand(
            {
                id,
                title: id === 'ui.discard' ? 'Discard close changes' : 'Save closing changes',
                palette: false
            },
            () => (closeChoice ? ui.answerChoice(closeChoice, index) : null)
        );
    ui.registerCommand({ id: 'ui.reload', title: 'Reload active file from disk...' }, async () => {
        const { document } = await ui.read();
        if (!document?.path) {
            await ui.update({ status: 'Save this buffer before reloading from disk' });
            return;
        }
        const discardChanges = document.dirty;
        if (
            discardChanges &&
            !(await ui.confirm({
                title: 'Reload from disk?',
                detail: `Discard unsaved changes in ${document.path}?`,
                accept: 'Reload'
            }))
        )
            return;
        const metadata = await request('workspace.reload', {
            id: document.id,
            baseRevision: document.revision,
            discardChanges
        });
        await ui.update({
            document: metadata,
            revealCaret: true,
            status: 'Reloaded from disk',
            workspaceNotice: { id: document.id, text: `Reloaded from disk: ${document.path}` }
        });
    });
    ui.registerView(
        { id: 'explorer', title: 'Explorer', icon: 'files', order: 0, outline: true },
        (_, options) => ({ pages: astra.operationPages('workspace.tree', {}, options) }),
        {
            show: () => ui.execute('ui.explorer'),
            async activate({ path }) {
                const focus = await ui.views.read('explorer');
                await ui.execute('workbench.open', { path });
                if (focus.focused) await ui.views.blur({ id: focus.id, focus: focus.focus });
            },
            project(model, options) {
                let truncated = false,
                    issues = 0;
                async function* rows() {
                    for await (const page of model.pages ?? [model]) {
                        options.signal?.throwIfAborted();
                        if (page.cancelled)
                            throw Object.assign(new Error('File tree cancelled'), {
                                name: 'AbortError'
                            });
                        truncated ||= page.truncated;
                        issues = Math.max(issues, page.issues?.length ?? 0);
                        for (const entry of page.entries) {
                            const path = entry.path.replaceAll('\\', '/');
                            const extension = path.slice(path.lastIndexOf('.'));
                            yield {
                                id: path,
                                label: path.slice(path.lastIndexOf('/') + 1),
                                depth: path.split('/').length - 1,
                                group: entry.directory,
                                icon: entry.directory
                                    ? 'folder'
                                    : ['.cpp', '.hpp', '.h', '.c'].includes(extension)
                                      ? 'code'
                                      : ['.js', '.ts', '.json'].includes(extension)
                                        ? 'braces'
                                        : 'document',
                                data: { path: entry.path }
                            };
                        }
                    }
                }
                return {
                    rows: rows(),
                    empty: 'No files in this workspace',
                    complete: () => ({
                        update: {
                            status: truncated
                                ? 'File tree reached its configured limit'
                                : issues
                                  ? 'Some workspace directories could not be read'
                                  : 'Workspace ready'
                        }
                    })
                };
            }
        }
    );
    ui.registerCommand({ id: 'ui.refreshTree', title: 'Refresh workspace files' }, () =>
        ui.refresh('explorer')
    );
    ui.registerCommand(
        { id: 'ui.explorer', title: 'Show file explorer', shortcut: 'Ctrl+Shift+E' },
        async () => {
            await ui.update({ sidebar: 'explorer', sidebarVisible: true });
            await ui.views.focus('explorer');
        }
    );
    ui.registerCommand(
        { id: 'ui.toggleSidebar', title: 'Toggle sidebar', shortcut: 'Ctrl+B' },
        async () => {
            const state = await ui.read({ immediate: true });
            await ui.update({ sidebarVisible: !state.sidebarVisible });
        }
    );
    ui.registerCommand({ id: 'ui.toggleOutline', title: 'Toggle outline' }, async () => {
        const state = await ui.read({ immediate: true });
        await ui.update({ outlineVisible: !state.outlineVisible });
    });
    ui.registerCommand({ id: 'ui.new', title: 'New file', shortcut: 'Ctrl+N' }, async () => {
        const document = await request('document.create', { text: '' });
        await ui.update({ document, activate: document.id, status: 'New file' });
    });
    ui.registerCommand({ id: 'workbench.open', title: 'Open workspace file' }, async ({ path }) => {
        const document = await request('workspace.open', { path });
        await ui.update({ document, activate: document.id, status: 'File opened' });
    });
    ui.registerCommand({ id: 'ui.save', title: 'Save file', shortcut: 'Ctrl+S' }, (parameters) =>
        save(false, parameters)
    );
    for (const [id, delta] of [
        ['ui.nextTab', 1],
        ['ui.previousTab', -1]
    ]) {
        ui.registerCommand({ id, title: delta > 0 ? 'Next tab' : 'Previous tab' }, async () => {
            const state = await ui.read({ immediate: true }),
                tabs = state.tabs;
            if (!tabs.length) return;
            const index = tabs.indexOf(state.document?.id);
            await ui.update({ activate: tabs[(index + delta + tabs.length) % tabs.length] });
        });
    }
}
async function save(saveAs, parameters = {}) {
    const options = { signal: parameters.signal };
    const state = await ui.read(parameters.id ? { id: parameters.id } : {}, options),
        document = state.document;
    if (!document) return null;
    let path = document.path;
    if (saveAs || !path) {
        path = await ui.input(
            { title: 'SAVE AS — WORKSPACE RELATIVE PATH', value: path || '' },
            options
        );
        if (!path) return null;
    }
    const saved = await request(
        'workspace.save',
        {
            id: document.id,
            baseRevision: document.revision,
            ...(saveAs || !document.path ? { path } : {})
        },
        options
    );
    await ui.update({ ...(!saved.closed ? { document: saved } : {}), status: 'Saved' });
    await ui.refresh('explorer');
    if (state.sidebar === 'git') await ui.refresh('git');
    await ui.update({ status: 'Saved' });
    return saved;
}
const closing = new Map();
let closeChoice,
    closeIdentity = 0;
async function close(parameters = {}) {
    const options = { signal: parameters.signal };
    const state = await ui.read(parameters.id ? { id: parameters.id } : {}, options),
        document = state.document;
    if (!document) return true;
    const pending = closing.get(document.id);
    if (pending) return await pending;
    const workflow = closeDocument(document, parameters.discardChanges === true, options);
    closing.set(document.id, workflow);
    try {
        return await workflow;
    } finally {
        if (closing.get(document.id) === workflow) closing.delete(document.id);
    }
}
async function closeDocument(document, discardChanges, options) {
    if (document.dirty && !discardChanges) {
        if (closeChoice) throw new Error('Another document is awaiting a close decision');
        const id = `close-${++closeIdentity}`;
        closeChoice = id;
        let decision;
        try {
            decision = await ui.choose(
                {
                    id,
                    title: 'Save changes before closing?',
                    detail: `${document.path?.split(/[\\/]/).at(-1) || 'Untitled'} has unsaved edits.`,
                    choices: [
                        { label: 'Save', value: 'save' },
                        { label: "Don't save", value: 'discard' },
                        { label: 'Cancel', value: 'cancel' }
                    ],
                    keys: ['', 'd', '']
                },
                options
            );
        } finally {
            if (closeChoice === id) closeChoice = undefined;
        }
        if (!decision || decision.value === 'cancel') return false;
        if (decision.value === 'discard') discardChanges = true;
        else {
            const saved = await ui.execute('ui.save', {
                id: document.id,
                ...(options.signal ? { signal: options.signal } : {})
            });
            options.signal?.throwIfAborted();
            if (!saved) return false;
            if (saved.closed) return true;
            if (saved.id !== document.id || !saved.revision)
                throw new Error('Save did not return the captured document revision');
            if (saved.dirty) {
                await ui.update({ status: 'New edits arrived while saving; document kept open' });
                return false;
            }
            document = saved;
        }
    }
    await request(
        'document.close',
        { id: document.id, baseRevision: document.revision, discardChanges },
        options
    );
    await ui.update({ closedDocument: document.id, status: 'Closed' });
    return true;
}
