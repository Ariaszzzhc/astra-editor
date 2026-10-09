import {ui} from 'astra/ui';
export function registerSearch() {
    ui.registerView({id:'search', title:'Search', icon:'search'}, async (parameters, options) => {
        const matches = [];
        const issues = [];
        let truncated = false, operationId;
        const started = await astra.request('workspace.search', parameters, {...options, raw:true});
        operationId = started.operationId;
        // Keep the service's search operation identity for guarded replacements.
        let offset = 0, complete = false;
        try {
            while (true) {
                const result = await astra.request('operation.get', {id:operationId, offset, limit:256}, options);
                if (result.state === 'pending') { await astra.sleep(8, options); continue; }
                if (result.state !== 'completed') throw Object.assign(new Error(result.error?.message ?? 'Search cancelled'), {name:result.state === 'cancelled' ? 'AbortError' : 'ServiceError'});
                matches.push(...result.result.matches);
                if (result.result.cancelled) throw Object.assign(new Error('Search cancelled; search again before replacing'), {name:'AbortError'});
                if (!offset) issues.push(...(result.result.issues ?? []));
                truncated ||= result.result.truncated;
                if (!result.page || result.page.nextOffset >= result.page.total) { complete = true; break; }
                offset = result.page.nextOffset;
            }
        } finally {
            if (!complete) await astra.request('operation.cancel', {id:operationId}, {raw:true});
        }
        return {matches, truncated, issues, operationId};
    });
    ui.registerCommand({id:'workbench.search', title:'Search workspace'}, async parameters => {
        await ui.update({sidebar:'search', sidebarVisible:true, status:'Searching workspace and unsaved buffers...'});
        await ui.refresh('search', parameters);
    });
    ui.registerCommand({id:'workbench.cancelView', title:'Cancel view operation'}, ({id}) => ui.cancel(id));
}
