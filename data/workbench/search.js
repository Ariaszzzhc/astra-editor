import { ui, request } from 'astra/ui';
let documentQuery = '',
    documentReplacement = '',
    documentSearchControl,
    documentForm,
    formIdentity = 0;
let formOptions = { caseSensitive: true, regex: false, wholeWord: false },
    navigation = Promise.resolve();
async function documentSearch(replace, parameters = {}) {
    documentSearchControl?.abort();
    const control = new AbortController();
    documentSearchControl = control;
    const options = { signal: control.signal };
    const abort = () => control.abort();
    parameters.signal?.addEventListener('abort', abort);
    if (parameters.signal?.aborted) control.abort();
    try {
        const query = parameters.query;
        if (!query) return false;
        documentQuery = query;
        const replacement = replace ? (parameters.replacement ?? '') : undefined;
        if (replace && replacement === null) return false;
        const { document } = await ui.read(parameters.id ? { id: parameters.id } : {}, options);
        if (!document) return false;
        await ui.update({
            status: replace ? 'Replacing in document...' : 'Finding in document...',
            error: ''
        });
        const result = await request(
            replace ? 'document.replaceAll' : 'document.find',
            {
                id: document.id,
                baseRevision: parameters.baseRevision ?? document.revision,
                query,
                caseSensitive: parameters.caseSensitive ?? true,
                regex: parameters.regex ?? false,
                wholeWord: parameters.wholeWord ?? false,
                ...(replace
                    ? { replacement }
                    : {
                          select: parameters.select ?? true,
                          direction: parameters.direction ?? 'forward',
                          wrap: parameters.wrap ?? true,
                          ...(parameters.begin === undefined ? {} : { begin: parameters.begin })
                      })
            },
            options
        );
        control.signal.throwIfAborted();
        if (parameters.component && !currentForm(parameters.component)) return false;
        await ui.update({
            ...(result.document ? { document: result.document, revealCaret: true } : {}),
            status: replace
                ? `Replaced ${result.replacements} match(es)`
                : result.match
                  ? result.wrapped
                      ? `Match found; wrapped to ${parameters.direction === 'backward' ? 'end' : 'beginning'}`
                      : 'Match found'
                  : 'No matches'
        });
        return result;
    } catch (error) {
        if (error.name === 'AbortError') {
            if (!parameters.component && documentSearchControl === control)
                await ui.update({ status: 'Document search cancelled' });
            return false;
        }
        throw error;
    } finally {
        parameters.signal?.removeEventListener('abort', abort);
        if (documentSearchControl === control) documentSearchControl = undefined;
    }
}
function currentForm(snapshot) {
    return documentForm?.id === snapshot.id && documentForm.version === snapshot.version;
}
function remember(values) {
    documentQuery = values.query;
    documentReplacement = values.replacement ?? documentReplacement;
    formOptions = {
        caseSensitive: values.caseSensitive,
        regex: values.regex,
        wholeWord: values.wholeWord
    };
}
async function openDocumentForm(replace) {
    const { document } = await ui.read();
    if (!document) return false;
    const existing = documentForm ? await ui.forms.read(documentForm.id) : null;
    if (existing) remember(existing.values);
    documentForm?.control?.abort();
    documentSearchControl?.abort();
    const id = `document-find-${++formIdentity}`;
    const spec = {
        id,
        title: replace ? 'Find and replace' : 'Find',
        focus: replace ? 'replacement' : 'query',
        fields: [
            {
                id: 'query',
                placeholder: 'Find',
                value: documentQuery,
                submit: 'next',
                shiftSubmit: 'previous'
            },
            ...(replace
                ? [
                      {
                          id: 'replacement',
                          placeholder: 'Replace',
                          value: documentReplacement,
                          submit: 'replaceAll'
                      }
                  ]
                : [])
        ],
        toggles: [
            {
                id: 'caseSensitive',
                label: 'Match case',
                value: formOptions.caseSensitive,
                shortcut: 'Alt+C'
            },
            {
                id: 'wholeWord',
                label: 'Whole word',
                value: formOptions.wholeWord,
                shortcut: 'Alt+W'
            },
            { id: 'regex', label: 'Regex', value: formOptions.regex, shortcut: 'Alt+R' }
        ],
        actions: [
            { id: 'previous', label: 'Previous' },
            { id: 'next', label: 'Next' },
            ...(replace ? [{ id: 'replaceAll', label: 'Replace all' }] : [])
        ]
    };
    const state = await ui.forms.open(spec);
    documentForm = { ...state, documentId: document.id };
    return state;
}
async function runForm(snapshot, action, preview = false) {
    if (!currentForm(snapshot)) return false;
    const owner = documentForm;
    owner.control?.abort();
    const control = new AbortController();
    owner.control = control;
    owner.preview = preview;
    try {
        if (preview) await astra.sleep(80, { signal: control.signal });
        if (!currentForm(snapshot)) return false;
        const { document } = await ui.read({}, { signal: control.signal });
        if (!document || document.id !== owner.documentId) return false;
        if (!snapshot.values.query) {
            await ui.forms.update({ ...snapshot, message: '', busy: false, error: false });
            return false;
        }
        await ui.forms.update({ ...snapshot, busy: true, error: false });
        const result = await documentSearch(action === 'replaceAll', {
            ...snapshot.values,
            id: document.id,
            select: !preview,
            direction: action === 'previous' ? 'backward' : 'forward',
            component: snapshot,
            signal: control.signal
        });
        if (result && currentForm(snapshot))
            await ui.forms.update({
                ...snapshot,
                busy: false,
                error: false,
                message:
                    action === 'replaceAll'
                        ? `Replaced ${result.replacements}`
                        : result.match
                          ? result.wrapped
                              ? 'Match found · wrapped'
                              : 'Match found'
                          : 'No matches'
            });
        if (!result && currentForm(snapshot) && owner.control === control)
            await ui.forms.update({ ...snapshot, busy: false });
        return result;
    } catch (error) {
        if (error.name !== 'AbortError' && currentForm(snapshot)) {
            await ui.forms.update({
                ...snapshot,
                busy: false,
                error: true,
                message: [...String(error.message)].slice(0, 120).join('')
            });
        }
        return false;
    } finally {
        if (owner.control === control) owner.control = undefined;
    }
}
export function registerSearch() {
    ui.registerCommand(
        { id: 'ui.find', title: 'Find in document', shortcut: 'Ctrl+F' },
        (parameters) =>
            parameters.query === undefined
                ? openDocumentForm(false)
                : documentSearch(false, parameters)
    );
    ui.registerCommand(
        { id: 'ui.replace', title: 'Replace all in document', shortcut: 'Ctrl+H' },
        (parameters) =>
            parameters.query === undefined
                ? openDocumentForm(true)
                : documentSearch(true, parameters)
    );
    ui.registerCommand(
        { id: 'workbench.cancelDocumentSearch', title: 'Cancel document search', palette: false },
        async () => {
            documentSearchControl?.abort();
            documentForm?.control?.abort();
            const owner = documentForm;
            if (owner) {
                const live = await ui.forms.read(owner.id);
                if (documentForm === owner) {
                    if (live) remember(live.values);
                    documentForm = undefined;
                }
                await ui.forms.close(owner.id);
            }
        }
    );
    astra.on('ui.form.changed', (snapshot) => {
        if (documentForm?.id !== snapshot.id) return;
        documentForm.control?.abort();
        documentSearchControl?.abort();
        documentForm.version = snapshot.version;
        documentForm.values = snapshot.values;
        remember(snapshot.values);
        return runForm(snapshot, 'next', true);
    });
    astra.on('ui.form.action', (snapshot) => {
        if (!currentForm(snapshot)) return;
        if (documentForm.preview) documentForm.control?.abort();
        const job = () => runForm(snapshot, snapshot.action);
        const pending = navigation.then(job, job);
        navigation = pending;
        return pending;
    });
    astra.on('ui.form.closed', (snapshot) => {
        if (documentForm?.id !== snapshot.id) return;
        remember(snapshot.values);
        documentForm.control?.abort();
        documentSearchControl?.abort();
        documentForm = undefined;
    });
    ui.registerView(
        { id: 'search', title: 'Search', icon: 'search', order: 1 },
        async (parameters, options) => {
            const matches = [];
            const issues = [];
            let truncated = false,
                operationId;
            const started = await astra.request('workspace.search', parameters, {
                ...options,
                raw: true
            });
            operationId = started.operationId;
            // Keep the service's search operation identity for guarded replacements.
            let offset = 0,
                complete = false;
            try {
                while (true) {
                    const result = await astra.request(
                        'operation.get',
                        { id: operationId, offset, limit: 256 },
                        options
                    );
                    if (result.state === 'pending') {
                        await astra.sleep(8, options);
                        continue;
                    }
                    if (result.state !== 'completed')
                        throw Object.assign(
                            new Error(result.error?.message ?? 'Search cancelled'),
                            { name: result.state === 'cancelled' ? 'AbortError' : 'ServiceError' }
                        );
                    matches.push(...result.result.matches);
                    if (result.result.cancelled)
                        throw Object.assign(
                            new Error('Search cancelled; search again before replacing'),
                            { name: 'AbortError' }
                        );
                    if (!offset) issues.push(...(result.result.issues ?? []));
                    truncated ||= result.result.truncated;
                    if (!result.page || result.page.nextOffset >= result.page.total) {
                        complete = true;
                        break;
                    }
                    offset = result.page.nextOffset;
                }
            } finally {
                if (!complete)
                    await astra.request('operation.cancel', { id: operationId }, { raw: true });
            }
            return { matches, truncated, issues, operationId };
        },
        {
            show: () => ui.execute('ui.search'),
            async project(model, options) {
                const rows = [];
                for (const match of model.matches) {
                    rows.push({
                        id: `${rows.length}`,
                        label: `${match.path.split(/[\\/]/).pop()}:${match.line + 1}`,
                        icon: 'document',
                        data: match
                    });
                    if (rows.length % 128 === 0) await astra.sleep(0, options);
                }
                return {
                    rows,
                    subtitle: `${rows.length} matches`,
                    empty: 'No search results',
                    metadata: model
                };
            },
            async activate(match) {
                const buffer = match.source === 'buffer';
                const document = await request(
                    buffer ? 'document.get' : 'workspace.open',
                    buffer ? { id: match.documentId, begin: 0, end: 0 } : { path: match.path }
                );
                await ui.update({ document, activate: document.id });
                if (
                    (buffer && document.revision !== match.revision) ||
                    (!buffer && document.dirty)
                ) {
                    await ui.update({
                        status: 'Search result changed; search again to locate the match'
                    });
                    return;
                }
                const selected = await request('document.setSelections', {
                    id: document.id,
                    baseRevision: document.revision,
                    selections: [{ anchor: match.begin, head: match.end }]
                });
                await ui.update({
                    document: selected,
                    scrollLine: { id: document.id, line: match.line }
                });
            }
        }
    );
    ui.registerCommand(
        { id: 'workbench.search', title: 'Search workspace' },
        async (parameters) => {
            await ui.update({
                sidebar: 'search',
                sidebarVisible: true,
                status: 'Searching workspace and unsaved buffers...'
            });
            await ui.refresh('search', parameters);
        }
    );
    ui.registerCommand(
        { id: 'workbench.cancelView', title: 'Cancel view operation' },
        async ({ id }) => {
            ui.cancel(id);
            await ui.update({ status: 'Cancelled' });
        }
    );
}
