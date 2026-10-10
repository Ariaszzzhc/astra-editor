import { fileIcons } from 'astra/icons';
const request = (method, params = {}, options) => astra.request(method, params, options);
export const ui = {
    read: (parameters = {}, options) => read(parameters, options),
    update: (model) => request('_ui.update', model),
    selectObject,
    icons: fileIcons,
    style: {
        read: (options) => request('_ui.style.read', {}, options),
        update: (patch, options) => updateStyle(patch, options)
    },
    views: {
        read: (id, options) => request('_ui.view.read', { id }, options),
        focus: (id, options) => request('_ui.view.focus', { id }, options),
        blur: (guards = {}, options) =>
            request('_ui.view.focus', { ...guards, editor: true }, options),
        select: (id, row, guards = {}, options) =>
            request('_ui.view.select', { ...guards, id, row }, options),
        async navigate(id, action, guards = {}, options) {
            const result = await request('_ui.view.navigate', { ...guards, id, action }, options);
            if (result?.activate) await activateView(result);
            return result;
        }
    },
    forms: {
        open: (spec) => request('_ui.form.open', spec),
        read: (id) => request('_ui.form.read', { id }),
        update: (parameters) => request('_ui.form.update', parameters),
        close: (id) => request('_ui.form.close', { id }),
        focus: (id, field, selectAll = true) => request('_ui.form.focus', { id, field, selectAll }),
        blur: (id) => request('_ui.form.blur', { id }),
        setValues: (parameters) => request('_ui.form.setValues', parameters)
    },
    clipboard: {
        read: () => request('_ui.clipboard.read'),
        write: (text) => request('_ui.clipboard.write', { text })
    },
    input: (spec, options) => dialog('input', spec, options),
    async confirm(spec, options) {
        const id = `dialog-${++dialogId}`,
            alias = spec.id;
        if (
            alias &&
            (typeof alias !== 'string' || alias.length > 128 || activeConfirmations.has(alias))
        )
            throw new TypeError('Invalid or active confirmation identity');
        if (alias) activeConfirmations.set(alias, id);
        try {
            return await dialog('confirm', spec, options, id);
        } finally {
            if (alias && activeConfirmations.get(alias) === id) activeConfirmations.delete(alias);
        }
    },
    async answerConfirm(id, accepted = true) {
        const nativeId = activeConfirmations.get(id);
        if (nativeId) return await request('_ui.dialog.answer', { id: nativeId, accepted });
        return null;
    },
    async choose(spec, options) {
        const choices = spec.choices.slice();
        const id = `dialog-${++dialogId}`,
            alias = spec.id;
        if (alias && (typeof alias !== 'string' || alias.length > 128 || activeChoices.has(alias)))
            throw new TypeError('Invalid or active choice identity');
        if (alias) activeChoices.set(alias, id);
        try {
            const index = await dialog(
                'choice',
                {
                    ...spec,
                    items: choices.map((choice) =>
                        typeof choice === 'string' ? choice : choice.label
                    )
                },
                options,
                id
            );
            return index === null ? null : choices[index];
        } finally {
            if (alias && activeChoices.get(alias) === id) activeChoices.delete(alias);
        }
    },
    async answerChoice(id, index) {
        const nativeId = activeChoices.get(id);
        if (nativeId) return await request('_ui.dialog.answer', { id: nativeId, index });
        return null;
    },
    async pick(spec, options) {
        if (typeof spec.items === 'function') return await dynamicPick(spec, options);
        const items = spec.items.slice();
        const index = await dialog(
            'pick',
            {
                ...spec,
                dynamic: false,
                items: items.map((item) => (typeof item === 'string' ? item : item.label)),
                iconPaths: items.map((item) =>
                    typeof item === 'string' ? '' : (item.iconPath ?? '')
                )
            },
            options
        );
        return index === null ? null : items[index];
    },
    showText: (spec, options) => dialog('text', spec, options),
    waitForIdle: (options) => fence('_ui.barrier', options),
    requestClose: (options) => fence('_ui.window.close', options),
    async withProgress(spec, work, options = {}) {
        const control = new AbortController(),
            signal = options.signal;
        if (signal?.aborted) throw cancelled('Progress cancelled');
        const abort = () => control.abort();
        signal?.addEventListener('abort', abort);
        const id = `dialog-${++dialogId}`;
        const input = dialog('progress', spec, { ...options, signal: control.signal }, id);
        // Both promises belong to this command and are observed even if the
        // component is dismissed while its operation is still completing.
        // Observe an open failure while awaiting the native acknowledgement.
        input.catch(() => {});
        let running;
        try {
            await request('_ui.dialog.present', { id });
            running = Promise.resolve().then(() => work({ signal: control.signal }));
            return await Promise.race([
                running,
                input.then(() => {
                    throw cancelled('Progress dismissed');
                })
            ]);
        } finally {
            control.abort();
            signal?.removeEventListener('abort', abort);
            await Promise.allSettled([input, running]);
        }
    },
    async execute(id, parameters = {}) {
        return await astra._invokeCommand(id, parameters);
    },
    async commands() {
        const state = await ui.read({ immediate: true });
        const service = new Set(state.serviceCommands || []);
        const client = new Set(astra._commandNames());
        const specs = astra.settings().workbench?.commands || {};
        return [...new Set([...client, ...service])].sort().map((id) => ({
            id,
            title: specs[id]?.title || id,
            shortcut: specs[id]?.shortcut || '',
            palette: specs[id]?.palette ?? !id.startsWith('workbench.'),
            source:
                service.has(id) && id.startsWith('ui.')
                    ? 'service'
                    : client.has(id)
                      ? 'client'
                      : 'service'
        }));
    },
    // Native actions handle text layout, focus, clipboard and modal input.
    // Product workflows call services directly and publish retained view state.
    native: (command, parameters = {}, options) =>
        command.startsWith('editor.')
            ? edit(command, parameters, options)
            : request('_ui.action', { command, parameters }, options),
    registerCommand(spec, handler) {
        if (!spec?.id || !spec?.title || typeof handler !== 'function')
            throw new TypeError('Invalid UI command');
        astra.configure({ workbench: { commands: { [spec.id]: spec } } });
        astra.registerCommand(spec.id, handler);
        if (spec.shortcut) astra.bind(spec.shortcut, spec.id);
    },
    registerView(spec, provider, handlers = {}) {
        if (
            typeof spec?.id !== 'string' ||
            !spec.id ||
            typeof spec?.title !== 'string' ||
            !spec.title ||
            typeof provider !== 'function'
        )
            throw new TypeError('Invalid view provider');
        if (!handlers || typeof handlers !== 'object') throw new TypeError('Invalid view handlers');
        const specification = Object.freeze({
            id: spec.id,
            title: spec.title,
            icon: spec.icon ?? 'files',
            order: spec.order ?? 0,
            outline: spec.outline ?? false
        });
        const callbacks = Object.freeze({ ...handlers });
        astra.configure({ workbench: { views: { [specification.id]: specification } } });
        generations.get(spec.id)?.abort();
        publishedViews.delete(spec.id);
        providers.set(specification.id, {
            spec: specification,
            provider,
            handlers: callbacks,
            parameters: {}
        });
    },
    async showView(id) {
        const view = providers.get(id);
        if (!view) throw new Error(`Unknown view: ${id}`);
        if (view.handlers.show) return await view.handlers.show();
        await ui.update({ sidebar: id, sidebarVisible: true });
        if (!publishedViews.has(id)) await ui.refresh(id, view.parameters);
    },
    async renderView(id, model, options = {}) {
        const view = providers.get(id);
        if (!view) throw new Error(`Unknown view: ${id}`);
        options.signal?.throwIfAborted();
        // Always receive this owner-local identity so cancellation can release it.
        const version = await request('_ui.view.begin', { spec: view.spec });
        let cancellation;
        const abort = () => {
            cancellation = request('_ui.view.abort', { id, version });
        };
        options.signal?.addEventListener('abort', abort);
        try {
            options.signal?.throwIfAborted();
            const presentation = view.handlers.project
                ? await view.handlers.project(model, options)
                : model;
            const rows = presentation?.rows ?? presentation;
            if (
                !rows ||
                typeof rows === 'string' ||
                (typeof rows[Symbol.asyncIterator] !== 'function' &&
                    typeof rows[Symbol.iterator] !== 'function')
            )
                throw new TypeError('View providers must supply iterable rows');
            let page = [],
                bytes = 2,
                totalBytes = 0,
                count = 0;
            for await (const row of rows) {
                options.signal?.throwIfAborted();
                const serialized = JSON.stringify(row);
                if (typeof serialized !== 'string')
                    throw new TypeError('View rows must be JSON objects');
                const length = astra.utf8ByteLength(serialized) + 1;
                if (length + 2 > 60000 || ++count > 100000)
                    throw new RangeError('View row or total row capacity exceeded');
                if (page.length && (page.length === 128 || bytes + length > 60000)) {
                    if (!(await request('_ui.view.append', { id, version, rows: page }, options)))
                        return false;
                    totalBytes += bytes;
                    page = [];
                    bytes = 2;
                    await astra.sleep(0, options);
                }
                if (totalBytes + bytes + length > 32 * 1024 * 1024)
                    throw new RangeError('View byte capacity exceeded');
                // A generator may reuse its row object. Retain only this bounded
                // page of JSON snapshots, not references it can mutate later.
                const prepared = JSON.parse(serialized);
                if (!prepared || typeof prepared !== 'object' || Array.isArray(prepared))
                    throw new TypeError('View rows must be JSON objects');
                page.push(prepared);
                bytes += length;
            }
            if (
                page.length &&
                !(await request('_ui.view.append', { id, version, rows: page }, options))
            )
                return false;
            options.signal?.throwIfAborted();
            let metadata = presentation;
            if (typeof presentation.complete === 'function') {
                const summary = await presentation.complete(options);
                if (
                    !summary ||
                    typeof summary !== 'object' ||
                    Array.isArray(summary) ||
                    Object.keys(summary).some(
                        (field) => !['subtitle', 'empty', 'update'].includes(field)
                    )
                )
                    throw new TypeError('Invalid view completion metadata');
                metadata = { ...presentation, ...summary };
            }
            options.signal?.throwIfAborted();
            const published = await request(
                '_ui.view.commit',
                {
                    id,
                    version,
                    subtitle: metadata.subtitle || '',
                    empty: metadata.empty || 'No items'
                },
                options
            );
            if (!published) return false;
            publishedViews.set(id, version);
            if (metadata.update)
                await ui.update({ ...metadata.update, view: id, viewVersion: version });
            return true;
        } finally {
            options.signal?.removeEventListener('abort', abort);
            await (cancellation || request('_ui.view.abort', { id, version }));
        }
    },
    async refresh(id, parameters = {}) {
        const view = providers.get(id);
        if (!view) throw new Error(`Unknown view: ${id}`);
        generations.get(id)?.abort();
        const controller = new AbortController();
        generations.set(id, controller);
        view.parameters = parameters;
        try {
            await ui.update({ busy: { [id]: true } });
            const model = await view.provider(parameters, { signal: controller.signal });
            if (generations.get(id) === controller && !controller.signal.aborted) {
                return await ui.renderView(id, model, { signal: controller.signal });
            }
        } catch (error) {
            if (error.name !== 'AbortError' && generations.get(id) === controller) throw error;
        } finally {
            if (generations.get(id) === controller) {
                generations.delete(id);
                await ui.update({ busy: { [id]: false } });
            }
        }
    },
    cancel(id) {
        generations.get(id)?.abort();
    }
};
const providers = new Map();
const publishedViews = new Map();
const generations = new Map();
const activeChoices = new Map();
const activeConfirmations = new Map();
let dialogId = 0;
let actionId = 0;
let readId = 0;
let fenceId = 0;
let styleId = 0;
async function selectObject(kind, parameters = {}, options = {}) {
    if (
        !['function', 'class', 'comment', 'string', 'block', 'parameter', 'heading'].includes(kind)
    ) {
        throw new TypeError('Unknown syntax object kind');
    }
    if (!parameters || typeof parameters !== 'object' || Array.isArray(parameters)) {
        throw new TypeError('Syntax selection parameters require an object');
    }
    for (const flag of ['inner', 'expand']) {
        if (parameters[flag] !== undefined && typeof parameters[flag] !== 'boolean') {
            throw new TypeError('Syntax selection flags must be boolean');
        }
    }
    const state = await ui.read(parameters.id ? { id: parameters.id } : {}, options),
        document = state.document;
    if (!document) return null;
    const revision = parameters.baseRevision ?? document.revision;
    const syntax = await request(
        'document.syntax',
        {
            id: document.id,
            baseRevision: revision,
            begin: 0,
            end: 0,
            includeOutline: false
        },
        options
    );
    if (syntax.state === 'unsupported') {
        await ui.update({ status: 'Syntax selection is unavailable for this document' });
        return null;
    }
    if (syntax.state === 'obsolete' || syntax.id !== document.id || syntax.revision !== revision) {
        await ui.update({ status: 'Syntax selection was superseded by a newer revision' });
        return null;
    }
    if (syntax.state !== 'ready') {
        throw new Error('Current syntax could not be prepared');
    }
    const selected = await request(
        'document.selectObject',
        {
            id: document.id,
            baseRevision: revision,
            kind,
            baseSelections: document.selections,
            inner: parameters.inner ?? false,
            expand: parameters.expand ?? false
        },
        options
    );
    await ui.update({ document: selected, revealCaretFor: document.id });
    return selected;
}
async function updateStyle(patch, options = {}) {
    const id = `style-${++styleId}`,
        signal = options.signal;
    let cancellation;
    const cancel = () => {
        cancellation = request('_ui.style.cancel', { id });
    };
    signal?.addEventListener('abort', cancel);
    try {
        return await request('_ui.style.update', { id, patch }, options);
    } catch (error) {
        await (cancellation || request('_ui.style.cancel', { id }));
        throw error;
    } finally {
        signal?.removeEventListener('abort', cancel);
        if (cancellation) await cancellation;
    }
}
function cancelled(message) {
    const error = new Error(message);
    error.name = 'AbortError';
    return error;
}
async function fence(method, options = {}) {
    const id = `wait-${++fenceId}`,
        signal = options.signal;
    let cancellation;
    const cancel = () => {
        cancellation = request('_ui.wait.cancel', { waitId: id });
    };
    signal?.addEventListener('abort', cancel);
    try {
        return await request(method, { waitId: id }, options);
    } catch (error) {
        await (cancellation || request('_ui.wait.cancel', { waitId: id }));
        throw error;
    } finally {
        signal?.removeEventListener('abort', cancel);
        if (cancellation) await cancellation;
    }
}
async function read(parameters, options = {}) {
    if (parameters.immediate) return await request('_ui.read', parameters, options);
    const id = `read-${++readId}`,
        signal = options.signal;
    let cancellation;
    const cancel = () => {
        cancellation = request('_ui.read.cancel', { readId: id });
    };
    signal?.addEventListener('abort', cancel);
    try {
        return await request('_ui.read', { ...parameters, readId: id }, options);
    } catch (error) {
        await (cancellation || request('_ui.read.cancel', { readId: id }));
        throw error;
    } finally {
        signal?.removeEventListener('abort', cancel);
        if (cancellation) await cancellation;
    }
}
async function edit(command, parameters, options = {}) {
    const id = `edit-${++actionId}`;
    const signal = options.signal;
    let cancellation;
    const cancel = () => {
        cancellation = request('_ui.action.cancel', { actionId: id });
    };
    signal?.addEventListener('abort', cancel);
    try {
        return await request('_ui.action', { command, parameters, actionId: id }, options);
    } finally {
        signal?.removeEventListener('abort', cancel);
        await (cancellation || request('_ui.action.cancel', { actionId: id }));
    }
}
async function dialog(kind, spec, options = {}, identity) {
    const id = identity || `dialog-${++dialogId}`;
    const signal = options.signal;
    let cancellation;
    const cancel = () => {
        cancellation = request('_ui.dialog.cancel', { id });
    };
    signal?.addEventListener('abort', cancel);
    try {
        return await request(
            '_ui.dialog',
            { ...spec, id, kind },
            { timeoutMs: 300000, ...options }
        );
    } finally {
        signal?.removeEventListener('abort', cancel);
        await (cancellation || request('_ui.dialog.cancel', { id }));
    }
}
astra.ui = ui;
const dynamicPicks = new Map();
async function dynamicPick(spec, options = {}) {
    const id = `dialog-${++dialogId}`;
    const record = { provider: spec.items, items: [], generation: '', published: '' };
    dynamicPicks.set(id, record);
    try {
        const result = await dialog(
            'pick',
            { ...spec, items: [], iconPaths: [], dynamic: true },
            options,
            id
        );
        if (result === null) return null;
        if (
            result.generation !== record.published ||
            !Number.isInteger(result.index) ||
            result.index < 0 ||
            result.index >= record.items.length
        ) {
            throw new Error('Picker returned an obsolete selection');
        }
        return record.items[result.index];
    } finally {
        record.control?.abort();
        if (dynamicPicks.get(id) === record) dynamicPicks.delete(id);
    }
}
// Race cancellation even for a user provider which ignores its signal. Observe
// both outcomes; its late continuation cannot publish to a newer query/dialog.
async function pickerWork(work, signal) {
    let abort;
    const cancelledWork = new Promise((_, reject) => {
        abort = () => reject(cancelled('Picker query cancelled'));
        signal.addEventListener('abort', abort);
        if (signal.aborted) abort();
    });
    try {
        return await Promise.race([work, cancelledWork]);
    } finally {
        signal.removeEventListener('abort', abort);
    }
}
astra.on('ui.picker.query', async ({ id, generation, query }) => {
    const record = dynamicPicks.get(id);
    if (!record || record.generation === generation) return;
    record.control?.abort();
    const control = new AbortController();
    record.control = control;
    record.generation = generation;
    const current = () =>
        dynamicPicks.get(id) === record &&
        record.generation === generation &&
        !control.signal.aborted;
    try {
        const output = await pickerWork(
            Promise.resolve().then(() => record.provider(query, { signal: control.signal })),
            control.signal
        );
        if (!current()) return;
        const items = (Array.isArray(output) ? output : output.items).slice();
        if (items.length > 4096) throw new RangeError('Picker exceeds 4096 results');
        const labels = items.map((item) => (typeof item === 'string' ? item : item.label));
        const iconPaths = items.map((item) =>
            typeof item === 'string' ? '' : (item.iconPath ?? '')
        );
        const message = Array.isArray(output) ? '' : (output.message ?? '');
        // Capture the original items before publication: native Enter may finish
        // the dialog before the publication acknowledgement reaches this task.
        record.items = items;
        record.published = generation;
        await request(
            '_ui.picker.publish',
            { id, generation, items: labels, iconPaths, message },
            { signal: control.signal }
        );
    } catch (error) {
        if (!current()) return;
        let message = String(error?.message ?? error)
            .replace(/[\r\n\0]/g, ' ')
            .slice(0, 512);
        if (/[\uD800-\uDBFF]$/.test(message)) message = message.slice(0, -1);
        record.items = [];
        record.published = generation;
        try {
            await request(
                '_ui.picker.publish',
                { id, generation, items: [], message, error: true },
                { signal: control.signal }
            );
        } catch (publicationError) {
            if (current()) throw publicationError;
        }
    }
});
async function activateView({ id, version, row }) {
    if (publishedViews.get(id) !== version) return;
    await providers.get(id)?.handlers.activate?.(row.data, row);
}
astra.on('ui.view.action', activateView);
for (const action of [
    'next',
    'previous',
    'first',
    'last',
    'pageUp',
    'pageDown',
    'left',
    'right',
    'toggle',
    'activate'
])
    ui.registerCommand(
        { id: `ui.view.${action}`, title: `View ${action}`, palette: false },
        ({ id, ...guards } = {}) => ui.views.navigate(id, action, guards)
    );
ui.registerCommand(
    { id: 'ui.focusSidebar', title: 'Focus sidebar rows', palette: false },
    async () => {
        await ui.update({ sidebarVisible: true });
        return await ui.views.focus();
    }
);
ui.registerCommand({ id: 'ui.focusEditor', title: 'Focus editor', palette: false }, () =>
    ui.views.blur()
);
ui.registerCommand(
    { id: 'workbench.showView', title: 'Show registered view', palette: false },
    ({ id }) => ui.showView(id)
);
ui.registerCommand(
    { id: 'workbench.refreshView', title: 'Refresh registered view', palette: false },
    ({ id }) => {
        const view = providers.get(id);
        if (view?.handlers.refresh) return view.handlers.refresh();
        return ui.refresh(id, view?.parameters);
    }
);
export { request };
