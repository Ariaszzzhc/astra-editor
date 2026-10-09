const request = (method, params = {}, options) => astra.request(method, params, options);
export const ui = {
    read: (parameters = {}) => request('_ui.read', parameters),
    update: model => request('_ui.update', model),
    clipboard: {
        read: () => request('_ui.clipboard.read'),
        write: text => request('_ui.clipboard.write', {text})
    },
    input: (spec, options) => dialog('input', spec, options),
    confirm: (spec, options) => dialog('confirm', spec, options),
    async pick(spec, options) {
        const items = spec.items.slice();
        const index = await dialog('pick', {...spec, items:items.map(item => typeof item === 'string' ? item : item.label)}, options);
        return index === null ? null : items[index];
    },
    showText: (spec, options) => dialog('text', spec, options),
    async execute(id, parameters = {}) {
        return await astra._invokeCommand(id, parameters);
    },
    async commands() {
        const state = await ui.read();
        const service = new Set(state.serviceCommands || []);
        const client = new Set(astra._commandNames());
        const specs = astra.settings().workbench?.commands || {};
        return [...new Set([...client, ...service])].sort().map(id => ({
            id, title:specs[id]?.title || id, shortcut:specs[id]?.shortcut || '',
            palette:specs[id]?.palette ?? !id.startsWith('workbench.'),
            source:service.has(id) && id.startsWith('ui.') ? 'service' : client.has(id) ? 'client' : 'service'
        }));
    },
    // Native actions handle text layout, focus, clipboard and modal input.
    // Product workflows call services directly and publish retained view state.
    native: (command, parameters = {}, options) => command.startsWith('editor.')
        ? edit(command, parameters, options) : request('_ui.action', {command, parameters}, options),
    registerCommand(spec, handler) {
        if (!spec?.id || !spec?.title || typeof handler !== 'function') throw new TypeError('Invalid UI command');
        astra.configure({workbench:{commands:{[spec.id]:spec}}});
        astra.registerCommand(spec.id, handler);
        if (spec.shortcut) astra.bind(spec.shortcut, spec.id);
    },
    registerView(spec, provider) {
        if (!spec?.id || !spec?.title || typeof provider !== 'function') throw new TypeError('Invalid view provider');
        providers.set(spec.id, provider);
        astra.configure({workbench:{views:{[spec.id]:spec}}});
    },
    async refresh(id, parameters = {}) {
        const provider = providers.get(id);
        if (!provider) throw new Error(`Unknown view: ${id}`);
        generations.get(id)?.abort();
        const controller = new AbortController();
        generations.set(id, controller);
        await ui.update({busy:{[id]:true}});
        try {
            const model = await provider(parameters, {signal:controller.signal});
            if (generations.get(id) === controller && !controller.signal.aborted) {
                await ui.update({view:id, model});
            }
        } catch (error) {
            if (error.name !== 'AbortError' && generations.get(id) === controller) throw error;
        } finally {
            if (generations.get(id) === controller) {
                generations.delete(id);
                await ui.update({busy:{[id]:false}});
            }
        }
    },
    cancel(id) { generations.get(id)?.abort(); }
};
const providers = new Map();
const generations = new Map();
let dialogId = 0;
let actionId = 0;
async function edit(command, parameters, options = {}) {
    const id = `edit-${++actionId}`;
    const signal = options.signal;
    let cancellation;
    const cancel = () => { cancellation = request('_ui.action.cancel', {actionId:id}); };
    signal?.addEventListener('abort', cancel);
    try { return await request('_ui.action', {command, parameters, actionId:id}, options); }
    finally {
        signal?.removeEventListener('abort', cancel);
        await (cancellation || request('_ui.action.cancel', {actionId:id}));
    }
}
async function dialog(kind, spec, options = {}) {
    const id = `dialog-${++dialogId}`;
    const signal = options.signal;
    let cancellation;
    const cancel = () => { cancellation = request('_ui.dialog.cancel', {id}); };
    signal?.addEventListener('abort', cancel);
    try { return await request('_ui.dialog', {...spec,id,kind}, {timeoutMs:300000,...options}); }
    finally {
        signal?.removeEventListener('abort', cancel);
        await (cancellation || request('_ui.dialog.cancel', {id}));
    }
}
astra.ui = ui;
export {request};
