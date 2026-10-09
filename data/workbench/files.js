import {ui, request} from 'astra/ui';
export function registerFiles() {
    ui.registerView({id:'explorer', title:'Explorer', icon:'files'}, async (_, options) => {
        const entries = [];
        let truncated = false;
        for await (const page of astra.operationPages('workspace.tree', {}, options)) {
            entries.push(...page.entries); truncated ||= page.truncated;
        }
        return {entries, truncated};
    });
    ui.registerCommand({id:'ui.refreshTree', title:'Refresh workspace files'}, () => ui.refresh('explorer'));
    ui.registerCommand({id:'ui.explorer', title:'Show file explorer', shortcut:'Ctrl+Shift+E'}, async () => {
        await ui.update({sidebar:'explorer', sidebarVisible:true});
    });
    ui.registerCommand({id:'ui.toggleSidebar', title:'Toggle sidebar', shortcut:'Ctrl+B'}, async () => {
        const state = await ui.read(); await ui.update({sidebarVisible:!state.sidebarVisible});
    });
    ui.registerCommand({id:'ui.toggleOutline', title:'Toggle outline'}, async () => {
        const state = await ui.read(); await ui.update({outlineVisible:!state.outlineVisible});
    });
    ui.registerCommand({id:'ui.new', title:'New file', shortcut:'Ctrl+N'}, async () => {
        const document = await request('document.create', {text:''});
        await ui.update({document, activate:document.id, status:'New file'});
    });
    ui.registerCommand({id:'workbench.open', title:'Open workspace file'}, async ({path}) => {
        const document = await request('workspace.open', {path});
        await ui.update({document, activate:document.id, status:'File opened'});
    });
    ui.registerCommand({id:'ui.save', title:'Save file', shortcut:'Ctrl+S'}, async () => {
        const state = await ui.read(), document = state.document;
        if (!document) return;
        if (!document.path) { await ui.native('ui.saveAs'); return; }
        const saved = await request('workspace.save', {id:document.id, baseRevision:document.revision});
        await ui.update({document:saved, status:'Saved'});
        await ui.refresh('explorer');
    });
    for (const [id, delta] of [['ui.nextTab',1],['ui.previousTab',-1]]) {
        ui.registerCommand({id, title:delta > 0 ? 'Next tab' : 'Previous tab'}, async () => {
            const state = await ui.read(), tabs = state.tabs;
            if (!tabs.length) return;
            const index = tabs.indexOf(state.document?.id);
            await ui.update({activate:tabs[(index + delta + tabs.length) % tabs.length]});
        });
    }
}
