import {ui, request} from 'astra/ui';
let documentQuery = '', documentSearchControl;
async function documentSearch(replace, parameters = {}) {
    if (documentSearchControl) return false;
    const control = new AbortController(); documentSearchControl = control;
    const options = {signal:control.signal};
    const abort = () => control.abort();
    parameters.signal?.addEventListener('abort', abort);
    if (parameters.signal?.aborted) control.abort();
    try {
        const query = parameters.query ?? await ui.input({title:replace ? 'REPLACE ALL IN DOCUMENT — FIND TEXT' : 'FIND IN DOCUMENT',value:documentQuery}, options);
        if (!query) return false;
        documentQuery = query;
        const replacement = replace ? parameters.replacement ?? await ui.input({title:'REPLACEMENT TEXT',value:''}, options) : undefined;
        if (replace && replacement === null) return false;
        const {document} = await ui.read(parameters.id ? {id:parameters.id} : {}, options);
        if (!document) return false;
        await ui.update({status:replace ? 'Replacing in document...' : 'Finding in document...',error:''});
        const result = await request(replace ? 'document.replaceAll' : 'document.find', {
            id:document.id, baseRevision:parameters.baseRevision ?? document.revision, query,
            caseSensitive:parameters.caseSensitive ?? true, regex:parameters.regex ?? false, wholeWord:parameters.wholeWord ?? false,
            ...(replace ? {replacement} : {select:true,wrap:parameters.wrap ?? true,...(parameters.begin === undefined ? {} : {begin:parameters.begin})})
        }, options);
        control.signal.throwIfAborted();
        await ui.update({...(result.document ? {document:result.document,revealCaret:true} : {}),
            status:replace ? `Replaced ${result.replacements} match(es)` : result.match ? (result.wrapped ? 'Match found; wrapped to beginning' : 'Match found') : 'No matches'});
        return result;
    } catch (error) {
        if (error.name === 'AbortError') { await ui.update({status:'Document search cancelled'}); return false; }
        throw error;
    } finally {
        parameters.signal?.removeEventListener('abort', abort);
        if (documentSearchControl === control) documentSearchControl = undefined;
    }
}
export function registerSearch() {
    ui.registerCommand({id:'ui.find',title:'Find in document',shortcut:'Ctrl+F'}, parameters => documentSearch(false,parameters));
    ui.registerCommand({id:'ui.replace',title:'Replace all in document',shortcut:'Ctrl+H'}, parameters => documentSearch(true,parameters));
    ui.registerCommand({id:'workbench.cancelDocumentSearch',title:'Cancel document search',palette:false}, () => {
        documentSearchControl?.abort();
    });
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
    ui.registerCommand({id:'workbench.cancelView', title:'Cancel view operation'}, async ({id}) => {
        ui.cancel(id); await ui.update({status:'Cancelled'});
    });
}
