import {ui, request} from 'astra/ui';
export function registerGit() {
    let diffController;
    ui.registerCommand({id:'workbench.gitDiff',title:'Open source-control comparison'},async ({path,staged=false}) => {
        if (!path) return;
        diffController?.abort();
        const controller=new AbortController();diffController=controller;
        try {
            const model=await request('git.diff',{path,staged},{signal:controller.signal});
            const text=model.binary ? 'Binary file — textual diff is unavailable.' : (model.patch || 'No changes in this comparison.');
            if (diffController===controller && !controller.signal.aborted) await ui.update({gitDiff:{path,staged,model,text}});
        } finally { if(diffController===controller)diffController=undefined; }
    });
    ui.registerCommand({id:'ui.gitDiff',title:'Show file diff'},async () => {
        const {document}=await ui.read();
        if(document?.path) await ui.execute('workbench.gitDiff',{path:document.path});
    });
    ui.registerCommand({id:'ui.gitCommit',title:'Commit staged changes'},async () => {
        const model=await request('git.status');
        await ui.update({view:'git',model});
        if (!model.entries.some(entry=>entry.staged!=='unchanged' && entry.staged!=='untracked')) {
            await ui.update({status:'No staged changes to commit'});return;
        }
        const message=await ui.input({title:'COMMIT STAGED CHANGES — COMMIT MESSAGE'});
        if(!message) return;
        await request('git.commit',{message});
        await ui.refresh('git');await ui.update({status:'Commit created'});
    });
    ui.registerCommand({id:'ui.gitDiscard',title:'Discard previewed working-tree changes...'},async () => {
        const state=await ui.read();
        if(!state.gitPath || state.gitStaged || !state.gitPreview) return;
        const previewId=state.gitPreview;
        await ui.update({dismissPrompt:true});
        if(!await ui.confirm({title:'Discard the previewed working tree change?',
            detail:`${state.gitPath}\nThis removes changes from disk. It cannot be undone here.`,accept:'Discard changes'})) return;
        const model=await request('git.discard',{previewId});
        await ui.update({view:'git',model});
        await ui.refresh('explorer');await ui.update({status:'Working tree change discarded'});
    });
    ui.registerView({id:'git', title:'Source Control', icon:'branch'}, (_, options) => request('git.status', {}, options));
    ui.registerCommand({id:'ui.gitStatus', title:'Source control: refresh changes', shortcut:'Ctrl+Shift+G'}, async () => {
        await ui.update({sidebar:'git', sidebarVisible:true});
        await ui.refresh('git');
    });
    for (const [id, method] of [['ui.gitStage','git.stage'],['ui.gitUnstage','git.unstage']]) {
        ui.registerCommand({id, title:method === 'git.stage' ? 'Stage file' : 'Unstage file'}, async () => {
            const state = await ui.read(), path = state.gitPath || state.document?.path;
            if (!path) return;
            const model = await request(method, {paths:[path]});
            await ui.update({view:'git', model, status:'Git index updated', dismissPrompt:true});
        });
    }
}
