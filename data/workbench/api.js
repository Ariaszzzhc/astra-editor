const request = (method, params = {}, options) => astra.request(method, params, options);
export const ui = {
    read: () => request('_ui.read'),
    update: model => request('_ui.update', model),
    // Native actions handle text layout, focus, clipboard and modal input.
    // Product workflows call services directly and publish retained view state.
    native: (command, parameters = {}) => request('_ui.action', {command, parameters}),
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
astra.ui = ui;
export {request};
