const request = (method, params = {}, options) => astra.request(method, params, options);
export const ui = {
    read: () => request('_ui.read'),
    update: model => request('_ui.update', model),
    clipboard: {
        read: () => request('_ui.clipboard.read'),
        write: text => request('_ui.clipboard.write', {text})
    },
    input: (spec, options) => dialog('input', spec, options),
    confirm: (spec, options) => dialog('confirm', spec, options),
    async execute(id, parameters = {}) {
        const handler = commands.get(id);
        if (!handler) throw new Error(`Unknown UI command: ${id}`);
        return await handler(parameters);
    },
    // Native actions handle text layout, focus, clipboard and modal input.
    // Product workflows call services directly and publish retained view state.
    native: (command, parameters = {}) => request('_ui.action', {command, parameters}),
    registerCommand(spec, handler) {
        if (!spec?.id || !spec?.title || typeof handler !== 'function') throw new TypeError('Invalid UI command');
        astra.configure({workbench:{commands:{[spec.id]:spec}}});
        commands.set(spec.id, handler);
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
const commands = new Map();
const generations = new Map();
let dialogId = 0;
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
