import {ui,request} from 'astra/ui';
export function registerLanguage() {
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
