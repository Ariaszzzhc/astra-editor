import {ui, request} from 'astra/ui';
export function registerFiles() {
    ui.registerCommand({id:'ui.quickOpen',title:'Open file by path',shortcut:'Ctrl+P'},async () => {
        const path = await ui.input({title:'OPEN WORKSPACE FILE'});
        if (!path) return;
        const document = await request('workspace.open', {path});
        await ui.update({document,activate:document.id,status:'File opened'});
    });
    ui.registerCommand({id:'ui.saveAs',title:'Save file as...',shortcut:'Ctrl+Shift+S'},() => save(true));
    ui.registerCommand({id:'ui.reload',title:'Reload active file from disk...'},async () => {
        const {document} = await ui.read();
        if (!document?.path) { await ui.update({status:'Save this buffer before reloading from disk'}); return; }
        const discardChanges = document.dirty;
        if (discardChanges && !await ui.confirm({title:'Reload from disk?',detail:`Discard unsaved changes in ${document.path}?`,accept:'Reload'})) return;
        const metadata = await request('workspace.reload',{id:document.id,baseRevision:document.revision,discardChanges});
        await ui.update({document:metadata,revealCaret:true,status:'Reloaded from disk',workspaceNotice:{id:document.id,text:`Reloaded from disk: ${document.path}`}});
    });
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
    ui.registerCommand({id:'ui.save', title:'Save file', shortcut:'Ctrl+S'}, () => save(false));
    for (const [id, delta] of [['ui.nextTab',1],['ui.previousTab',-1]]) {
        ui.registerCommand({id, title:delta > 0 ? 'Next tab' : 'Previous tab'}, async () => {
            const state = await ui.read(), tabs = state.tabs;
            if (!tabs.length) return;
            const index = tabs.indexOf(state.document?.id);
            await ui.update({activate:tabs[(index + delta + tabs.length) % tabs.length]});
        });
    }
}
async function save(saveAs) {
    const {document} = await ui.read();
    if (!document) return;
    let path = document.path;
    if (saveAs || !path) {
        path = await ui.input({title:'SAVE AS — WORKSPACE RELATIVE PATH',value:path || ''});
        if (!path) return;
    }
    const saved = await request('workspace.save',{id:document.id,baseRevision:document.revision,...((saveAs || !document.path) ? {path} : {})});
    await ui.update({document:saved,status:'Saved'});
    await ui.refresh('explorer');
}
