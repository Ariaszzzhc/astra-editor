import {ui,request} from 'astra/ui';
let interactive;
async function languageResult(method, present) {
    interactive?.abort();
    const controller = new AbortController(); interactive = controller;
    const options = {signal:controller.signal};
    try {
        const {document} = await ui.read(); if (!document) return;
        await ui.update({languageActive:{id:document.id,active:true}});
        const offset = document.selections[0].head;
        const result = await request('language.request', {id:document.id,baseRevision:document.revision,method,byteOffset:offset}, options);
        const current = (await ui.read()).document;
        if (result.obsolete || current?.id !== document.id || current.revision !== result.revision) {
            await ui.update({status:'Discarded a language result for an older revision'}); return;
        }
        await present(result, document, offset, options);
    } catch (error) { if (error.name !== 'AbortError') throw error; }
    finally { if (interactive === controller) interactive = undefined; }
}
async function navigate(item, options) {
    if (!item.path) { await ui.update({status:`Location is outside the workspace: ${item.uri || ''}`}); return; }
    let document = await request('workspace.open', {path:item.path}, options);
    if (item.targetRevision !== undefined && item.targetRevision !== document.revision) {
        await ui.update({status:'Navigation target changed; request the location again'}); return;
    }
    if (item.byteOffset !== undefined) {
        document = await request('document.setSelections', {id:document.id,baseRevision:document.revision,
            selections:[{anchor:item.byteOffset,head:item.endByteOffset ?? item.byteOffset}]}, options);
        await ui.update({document,activate:document.id,revealCaret:true,status:'Location opened'});
    } else await ui.update({document,activate:document.id,scrollLine:{id:document.id,line:item.line || 0},status:'Location opened'});
}
export function registerLanguage() {
    ui.registerCommand({id:'ui.completion',title:'Trigger completion',shortcut:'Ctrl+Space'},() => languageResult('completion',async (result, document, offset, options) => {
        const items = Array.isArray(result.result) ? result.result : result.result?.items || [];
        if (!items.length) { await ui.update({status:'No completion items'}); return; }
        const choice = await ui.pick({title:'COMPLETION — TYPE TO FILTER · ENTER TO INSERT',items:items.map(item=>({label:item.label,item}))}, options);
        if (!choice) return;
        const current = (await ui.read()).document;
        if (current?.id !== document.id || current.revision !== result.revision || current.selections[0].head !== offset) {
            await ui.update({status:'Completion became stale; request it again'}); return;
        }
        const metadata = await request('language.applyCompletion', {id:document.id,baseRevision:result.revision,byteOffset:offset,completionItem:choice.item}, options);
        await ui.update({document:metadata,revealCaret:true,status:'Completion applied'});
    }));
    ui.registerCommand({id:'ui.hover',title:'Show hover information',shortcut:'Ctrl+K'},() => languageResult('hover',async (result, _, __, options) => {
        const contents = result.result?.contents;
        const parts = Array.isArray(contents) ? contents : contents == null ? [] : [contents];
        const text = parts.map(part=>typeof part === 'string' ? part : part.value || '').join('\n');
        if (!text) { await ui.update({status:'No hover information'}); return; }
        await ui.showText({title:'LANGUAGE INFORMATION — Esc to close · arrows to scroll',text}, options);
    }));
    for (const [method,shortcut,title] of [['definition','F12','Go to definition'],['references','Shift+F12','Find all references']]) {
        ui.registerCommand({id:`ui.${method}`,title,shortcut},() => languageResult(method,async (result, _, __, options) => {
            const locations = result.resolvedLocations || [];
            if (!locations.length) { await ui.update({status:'No locations found'}); return; }
            const choice = locations.length === 1 ? {item:locations[0]} : await ui.pick({title:'LANGUAGE LOCATIONS — ENTER TO OPEN',
                items:locations.map(item=>({label:`${item.path || item.uri}:${(item.line || 0)+1}`,item}))}, options);
            if (choice) await navigate(choice.item, options);
        }));
    }
    ui.registerCommand({id:'ui.diagnostics',title:'Show document diagnostics',shortcut:'Ctrl+Shift+M'},async () => {
        const {document} = await ui.read(); if (!document) return;
        const result = await request('language.diagnostics', {id:document.id});
        if (result.revision !== document.revision) { await ui.update({status:'Diagnostics changed; request them again'}); return; }
        const text = result.diagnostics.map(item=>`${item.range.start.line+1}:${item.range.start.character+1}  ${item.message}`).join('\n') ||
            (result.pending ? 'Waiting for language diagnostics...' : result.reason || 'No diagnostics');
        await ui.showText({title:'DOCUMENT DIAGNOSTICS — Esc to close · arrows to scroll',text});
    });
    ui.registerCommand({id:'ui.format',title:'Format document',shortcut:'Shift+Alt+F'},async () => {
        const {document}=await ui.read();if(!document)return;
        const result=await request('language.request',{id:document.id,baseRevision:document.revision,method:'formatting'});
        if(result.obsolete){await ui.update({status:'Discarded a language result for an older revision'});return;}
        if(!result.result?.length){await ui.update({status:'Document is already formatted'});return;}
        const metadata=await request('language.applyTextEdits',{id:document.id,baseRevision:result.revision,edits:result.result});
        await ui.update({document:metadata,revealCaret:true,status:'Document formatted'});
    });
    ui.registerCommand({id:'ui.rename',title:'Rename symbol',shortcut:'F2'},async () => {
        const {document}=await ui.read();if(!document)return;
        const newName=await ui.input({title:'RENAME SYMBOL — NEW NAME'});if(!newName)return;
        const result=await request('language.request',{id:document.id,baseRevision:document.revision,method:'rename',
            byteOffset:document.selections[0].head,newName});
        if(result.obsolete){await ui.update({status:'Discarded a language result for an older revision'});return;}
        if(!result.planId || !result.documents?.length){await ui.update({status:'No rename edits'});return;}
        const accepted=await ui.confirm({title:'Apply symbol rename?',accept:'Apply rename',
            detail:`${result.documents.length} file(s) will become edited buffers. Save afterward to persist changes.`,
            items:result.documents.map(file=>`${file.path} (${file.editCount} edits)`)});
        if(!accepted)return;
        const applied=await request('language.applyWorkspaceEdit',{planId:result.planId});
        await ui.update({documents:applied.documents,status:'Rename applied to buffers; save files to persist'});
    });
}
