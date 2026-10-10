import { ui } from 'astra/ui';

let identity = 0;
let owner;
let write;
let scope = {};
let remembered = {
    query: '',
    replacement: '',
    caseSensitive: true,
    regex: false,
    wholeWord: false
};

function current(snapshot) {
    return owner?.id === snapshot.id && owner.version === snapshot.version;
}
function query(values) {
    return {
        query: values.query,
        caseSensitive: values.caseSensitive,
        regex: values.regex,
        wholeWord: values.wholeWord
    };
}
function abortError(message) {
    return Object.assign(new Error(message), { name: 'AbortError' });
}
function fetchSearch(parameters, options = {}) {
    const model = { count: 0, fileCount: 0, paths: [], issues: [], truncated: false };
    model.matches = matches();
    return model;
    async function* matches() {
        // Receive the identity even if input changes while the service starts work.
        const started = await astra.request('workspace.search', parameters, { raw: true });
        model.operationId = started.operationId;
        let offset = 0;
        let complete = false;
        let total;
        let paginated;
        const paths = new Set();
        try {
            for (;;) {
                options.signal?.throwIfAborted();
                const page = await astra.request(
                    'operation.get',
                    { id: model.operationId, offset, limit: 256 },
                    options
                );
                if (page.state === 'pending') {
                    await astra.sleep(8, options);
                    continue;
                }
                if (page.state !== 'completed') {
                    if (page.state === 'cancelled') throw abortError('Search cancelled');
                    throw new Error(page.error?.message ?? 'Workspace search failed');
                }
                if (page.result.cancelled)
                    throw abortError('Search cancelled; search again before replacing');
                if (!Array.isArray(page.result.matches) || !Array.isArray(page.result.issues))
                    throw new Error('Invalid search result collections');
                if (!offset) {
                    model.issues = page.result.issues;
                    model.fileCount = page.result.fileCount;
                    if (!Number.isInteger(model.fileCount) || model.fileCount < 0)
                        throw new Error('Invalid search file count');
                    paginated = !!page.page;
                    total = page.page?.total ?? page.result.matches.length;
                }
                if (
                    page.result.fileCount !== model.fileCount ||
                    !!page.page !== paginated ||
                    (page.page &&
                        (!Number.isInteger(page.page.total) ||
                            page.page.total !== total ||
                            page.page.offset !== offset ||
                            page.page.nextOffset !== offset + page.result.matches.length ||
                            page.page.nextOffset > total))
                )
                    throw new Error('Search page metadata changed or did not advance');
                model.truncated ||= page.result.truncated;
                for (const match of page.result.matches) {
                    if (paths.size < 200 && !paths.has(match.path)) {
                        paths.add(match.path);
                        model.paths.push(match.path);
                    }
                    ++model.count;
                    yield match;
                }
                if (!page.page || page.page.nextOffset >= page.page.total) {
                    if (
                        model.count !== total ||
                        model.fileCount > model.count ||
                        model.paths.length > model.fileCount ||
                        (model.paths.length < 200 && model.paths.length !== model.fileCount)
                    )
                        throw new Error('Search totals do not match the received rows');
                    complete = true;
                    return;
                }
                if (!Number.isInteger(page.page.nextOffset) || page.page.nextOffset <= offset)
                    throw new Error('Search page did not advance');
                offset = page.page.nextOffset;
            }
        } finally {
            if (!complete)
                await astra.request('operation.cancel', { id: model.operationId }, { raw: true });
        }
    }
}
async function live(snapshot) {
    if (!current(snapshot)) return false;
    const state = await ui.forms.read(snapshot.id);
    return current(snapshot) && state?.version === snapshot.version;
}
async function applyReplacement(model, snapshot) {
    const task = { cancelled: false, id: undefined };
    write = task;
    try {
        const started = await astra.request(
            'workspace.replace',
            {
                searchId: model.operationId,
                replacement: snapshot.values.replacement
            },
            { raw: true }
        );
        task.id = started.operationId;
        if (task.cancelled) await astra.request('operation.cancel', { id: task.id }, { raw: true });
        let result;
        for (;;) {
            const page = await astra.request('operation.get', { id: task.id }, { raw: true });
            if (page.state === 'pending') {
                await astra.sleep(8);
                continue;
            }
            if (page.state !== 'completed')
                throw new Error(
                    `${page.error?.message ?? 'Replacement did not complete'}; applied counts are unavailable. Search again before retrying`
                );
            result = page.result;
            break;
        }
        const modified = result.modifiedBuffers ?? [];
        const pending = result.pending.length + (result.pendingBuffers?.length ?? 0);
        let message = `Replaced in ${result.applied.length} disk file(s) and ${modified.length} unsaved buffer(s)`;
        if (pending || result.issues.length || result.cancelled)
            message += `; ${pending} pending, ${result.issues.length} issue(s)${result.cancelled ? ', cancelled' : ''}. Search again before retrying`;
        await ui.update({ documents: modified, status: message, notice: message });
        if (await live(snapshot))
            await ui.forms.update({
                ...snapshot,
                busy: false,
                message: `Replaced ${result.applied.length} file(s), ${modified.length} buffer(s)`,
                error: !!(pending || result.issues.length || result.cancelled)
            });
        try {
            await ui.refresh('explorer');
        } catch (error) {
            await ui.update({
                error: `Replacement completed; file refresh failed: ${error.message}`
            });
        }
        return result;
    } finally {
        if (write === task) write = undefined;
    }
}
async function run(snapshot, replace = false, preview = false) {
    if (!current(snapshot) || write) return false;
    const record = owner;
    record.control?.abort();
    const control = new AbortController();
    record.control = control;
    const options = { signal: control.signal };
    let acceptedWrite = false;
    try {
        if (preview) await astra.sleep(200, options);
        if (!(await live(snapshot))) return false;
        if (!snapshot.values.query) {
            await ui.renderView('search', { matches: [], issues: [], truncated: false }, options);
            await ui.forms.update({ ...snapshot, busy: false, message: '', error: false });
            return false;
        }
        await ui.forms.update({ ...snapshot, busy: true, message: 'Searching…', error: false });
        const model = fetchSearch({ ...scope, ...query(snapshot.values) }, options);
        if (!(await live(snapshot))) return false;
        if (!(await ui.renderView('search', model, options))) return false;
        const unsafe = model.truncated || model.issues.length;
        const message = unsafe
            ? model.truncated
                ? 'Search limit reached; refine query'
                : 'Some files could not be read'
            : `${model.count} match(es)`;
        await ui.forms.update({ ...snapshot, busy: false, message, error: !!unsafe });
        if (!replace || unsafe || !model.count)
            return {
                operationId: model.operationId,
                matchCount: model.count,
                fileCount: model.fileCount,
                paths: model.paths,
                truncated: model.truncated,
                issues: model.issues
            };
        const confirmation = `workspace-replace-${++identity}`;
        record.confirmation = confirmation;
        let accepted;
        try {
            accepted = await ui.confirm(
                {
                    id: confirmation,
                    title: `Replace ${model.count} match(es)?`,
                    detail: `Replace with ${JSON.stringify(snapshot.values.replacement)} in ${model.fileCount} file(s). Unsaved buffers remain unsaved and undoable.`,
                    items: model.paths,
                    accept: 'Replace all',
                    cancel: 'Cancel'
                },
                options
            );
        } finally {
            if (record.confirmation === confirmation) record.confirmation = undefined;
        }
        if (!accepted || !(await live(snapshot))) return false;
        control.signal.throwIfAborted();
        await ui.forms.update({ ...snapshot, busy: true, message: 'Replacing…', error: false });
        if (!(await live(snapshot))) return false;
        control.signal.throwIfAborted();
        // Only explicit cancellation stops accepted writes. Continue polling to
        // report partial application even if the form is closed or its text changes.
        acceptedWrite = true;
        return await applyReplacement(model, snapshot);
    } catch (error) {
        if (
            error.name !== 'AbortError' &&
            (acceptedWrite || (!control.signal.aborted && (await live(snapshot))))
        ) {
            await ui.update({
                error: error.message,
                status: 'Workspace search or replacement failed'
            });
            if (await live(snapshot))
                await ui.forms.update({
                    ...snapshot,
                    busy: false,
                    error: true,
                    message: [...String(error.message)].slice(0, 120).join('')
                });
        }
        return false;
    } finally {
        if (record.control === control) {
            record.control = undefined;
            if (await live(snapshot)) await ui.forms.update({ ...snapshot, busy: false });
        }
    }
}
async function openForm(replace = false) {
    await ui.update({ sidebar: 'search', sidebarVisible: true });
    const existing = owner ? await ui.forms.read(owner.id) : null;
    if (existing) {
        owner.version = existing.version;
        owner.values = existing.values;
        remembered = { ...existing.values };
        return await ui.forms.focus(owner.id, replace ? 'replacement' : 'query');
    }
    owner?.control?.abort();
    const state = await ui.forms.open({
        id: `workspace-search-${++identity}`,
        view: 'search',
        title: 'Workspace search',
        focus: replace ? 'replacement' : 'query',
        fields: [
            { id: 'query', placeholder: 'Search', value: remembered.query, submit: 'search' },
            {
                id: 'replacement',
                placeholder: 'Replace',
                value: remembered.replacement,
                submit: 'replaceAll'
            }
        ],
        toggles: [
            {
                id: 'caseSensitive',
                label: 'Match case',
                value: remembered.caseSensitive,
                shortcut: 'Alt+C'
            },
            {
                id: 'wholeWord',
                label: 'Whole word',
                value: remembered.wholeWord,
                shortcut: 'Alt+W'
            },
            { id: 'regex', label: 'Regex', value: remembered.regex, shortcut: 'Alt+R' }
        ],
        actions: [
            { id: 'search', label: 'Search' },
            { id: 'replaceAll', label: 'Replace all' },
            { id: 'stop', label: 'Stop' }
        ]
    });
    owner = { ...state };
    return state;
}
async function cancel({ onlyFocused = false, shutdown = false } = {}) {
    const record = owner;
    if (onlyFocused) {
        if (!record) return false;
        const state = await ui.read({ immediate: true });
        if (state.focusedForm?.id !== record?.id && !record?.confirmation) return false;
    }
    record?.control?.abort();
    ui.cancel('search');
    if (record) {
        const state = await ui.forms.read(record.id);
        if (state && owner === record) {
            record.version = state.version;
            record.values = state.values;
            remembered = { ...state.values };
            await ui.forms.update({ ...state, busy: false, message: 'Cancelled', error: false });
            await ui.forms.blur(record.id);
        }
    }
    if (write && !shutdown) {
        write.cancelled = true;
        if (write.id) await astra.request('operation.cancel', { id: write.id }, { raw: true });
    }
    return true;
}
export function registerWorkspaceSearch() {
    ui.registerView({ id: 'search', title: 'Search', icon: 'search', order: 1 }, fetchSearch, {
        show: () => openForm(),
        refresh: async () => {
            const state = await openForm();
            return await run(state);
        },
        project(model, options) {
            let count = 0;
            async function* rows() {
                for await (const match of model.matches) {
                    options.signal?.throwIfAborted();
                    yield {
                        id: `${count++}`,
                        label: `${match.path.split(/[\\/]/).pop()}:${match.line + 1}`,
                        icon: 'document',
                        data: match
                    };
                }
            }
            return {
                rows: rows(),
                empty: 'No search results',
                complete: () => ({
                    subtitle: `${count} matches`,
                    update: {
                        status: `${count} workspace match(es)`,
                        error: model.truncated
                            ? 'Search reached its limit; refine the query before replacing'
                            : model.issues.length
                              ? 'Search could not read some files; refine the scope before replacing'
                              : ''
                    }
                })
            };
        },
        async activate(match) {
            const focus = await ui.views.read('search');
            const buffer = match.source === 'buffer';
            const document = await astra.request(
                buffer ? 'document.get' : 'workspace.open',
                buffer ? { id: match.documentId, begin: 0, end: 0 } : { path: match.path }
            );
            await ui.update({ document, activate: document.id });
            if ((buffer && document.revision !== match.revision) || (!buffer && document.dirty)) {
                await ui.update({
                    status: 'Search result changed; search again to locate the match'
                });
                if (focus.focused) await ui.views.blur({ id: focus.id, focus: focus.focus });
                return;
            }
            const selected = await astra.request('document.setSelections', {
                id: document.id,
                baseRevision: document.revision,
                selections: [{ anchor: match.begin, head: match.end }]
            });
            await ui.update({
                document: selected,
                scrollLine: { id: document.id, line: match.line }
            });
            if (focus.focused) await ui.views.blur({ id: focus.id, focus: focus.focus });
        }
    });
    for (const [id, title, shortcut, replace] of [
        ['ui.search', 'Search workspace and unsaved buffers', 'Ctrl+Shift+F', false],
        ['ui.workspaceReplace', 'Replace across workspace', 'Ctrl+Shift+H', true]
    ])
        ui.registerCommand({ id, title, shortcut }, () => openForm(replace));
    ui.registerCommand(
        { id: 'workbench.search', title: 'Search workspace' },
        async (parameters) => {
            const state = await openForm();
            const values = { ...remembered };
            for (const field of Object.keys(values))
                if (parameters[field] !== undefined) values[field] = parameters[field];
            scope = {};
            for (const field of ['scope', 'maxMatches', 'includeIgnored'])
                if (parameters[field] !== undefined) scope[field] = parameters[field];
            const next = await ui.forms.setValues({ ...state, values });
            if (!next || !owner || next.id !== owner.id) return false;
            owner.version = next.version;
            owner.values = next.values;
            remembered = { ...next.values };
            return await run(next);
        }
    );
    ui.registerCommand(
        { id: 'workbench.cancelWorkspaceSearch', title: 'Cancel workspace search', palette: false },
        cancel
    );
    ui.registerCommand(
        { id: 'workbench.cancelView', title: 'Cancel view operation' },
        async ({ id }) => {
            if (id === 'search') return await cancel();
            ui.cancel(id);
            await ui.update({ status: 'Cancelled' });
        }
    );
    ui.registerCommand(
        { id: 'ui.confirmReplace', title: 'Confirm workspace replacement', palette: false },
        async () => {
            if (owner?.confirmation) return await ui.answerConfirm(owner.confirmation, true);
            return false;
        }
    );
    for (const [id, field, shortcut] of [
        ['ui.toggleSearchCase', 'caseSensitive', 'Alt+C'],
        ['ui.toggleSearchRegex', 'regex', 'Alt+R'],
        ['ui.toggleSearchWholeWord', 'wholeWord', 'Alt+W']
    ])
        ui.registerCommand({ id, title: `Search: toggle ${field}`, shortcut }, async () => {
            const state = await openForm();
            return await ui.forms.setValues({
                ...state,
                values: { [field]: !state.values[field] }
            });
        });
    astra.on('ui.form.changed', (snapshot) => {
        if (owner?.id !== snapshot.id || owner.version === snapshot.version) return;
        owner.control?.abort();
        owner.version = snapshot.version;
        owner.values = snapshot.values;
        remembered = { ...snapshot.values };
        return run(snapshot, false, true);
    });
    astra.on('ui.form.action', (snapshot) => {
        if (!current(snapshot)) return;
        if (snapshot.action === 'stop') return cancel();
        return run(snapshot, snapshot.action === 'replaceAll');
    });
    astra.on('ui.form.closed', (snapshot) => {
        if (owner?.id !== snapshot.id) return;
        remembered = { ...snapshot.values };
        owner.control?.abort();
        owner = undefined;
    });
}
