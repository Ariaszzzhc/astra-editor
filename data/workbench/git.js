import {ui, request} from 'astra/ui';
export function registerGit() {
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
