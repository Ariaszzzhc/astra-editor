import { ui, request } from 'astra/ui';
export function registerGit() {
    let diffController;
    ui.registerCommand(
        { id: 'workbench.gitDiff', title: 'Open source-control comparison' },
        async ({ path, staged = false }) => {
            if (!path) return;
            diffController?.abort();
            const controller = new AbortController();
            diffController = controller;
            try {
                const model = await request(
                    'git.diff',
                    { path, staged },
                    { signal: controller.signal }
                );
                const text = model.binary
                    ? 'Binary file — textual diff is unavailable.'
                    : model.patch || 'No changes in this comparison.';
                if (diffController === controller && !controller.signal.aborted)
                    await ui.update({ gitDiff: { path, staged, model, text } });
            } finally {
                if (diffController === controller) diffController = undefined;
            }
        }
    );
    ui.registerCommand({ id: 'ui.gitDiff', title: 'Show file diff' }, async () => {
        const { document } = await ui.read();
        if (document?.path) await ui.execute('workbench.gitDiff', { path: document.path });
    });
    ui.registerCommand({ id: 'ui.gitCommit', title: 'Commit staged changes' }, async () => {
        const model = await request('git.status');
        if (
            !model.entries.some(
                (entry) => entry.staged !== 'unchanged' && entry.staged !== 'untracked'
            )
        ) {
            await ui.update({ status: 'No staged changes to commit' });
            return;
        }
        await ui.renderView('git', model);
        const message = await ui.input({ title: 'COMMIT STAGED CHANGES — COMMIT MESSAGE' });
        if (!message) return;
        await request('git.commit', { message });
        await ui.refresh('git');
        await ui.update({ status: 'Commit created' });
    });
    ui.registerCommand(
        { id: 'ui.gitDiscard', title: 'Discard previewed working-tree changes...' },
        async () => {
            const state = await ui.read();
            if (!state.gitPath || state.gitStaged || !state.gitPreview) return;
            const previewId = state.gitPreview;
            await ui.update({ dismissPrompt: true });
            if (
                !(await ui.confirm({
                    title: 'Discard the previewed working tree change?',
                    detail: `${state.gitPath}\nThis removes changes from disk. It cannot be undone here.`,
                    accept: 'Discard changes'
                }))
            )
                return;
            const model = await request('git.discard', { previewId });
            await ui.renderView('git', model);
            await ui.refresh('explorer');
            await ui.update({ status: 'Working tree change discarded' });
        }
    );
    ui.registerView(
        { id: 'git', title: 'Source Control', icon: 'branch', order: 2 },
        (_, options) => request('git.status', {}, options),
        {
            show: () => ui.execute('ui.gitStatus'),
            activate: (entry) =>
                ui.execute('workbench.gitDiff', {
                    path: entry.path,
                    staged: entry.unstaged === 'unchanged'
                }),
            async project(model, options) {
                const rows = [];
                for (const entry of model.entries) {
                    const change = entry.unstaged !== 'unchanged' ? entry.unstaged : entry.staged;
                    rows.push({
                        id: entry.path,
                        label: entry.path.split(/[\\/]/).pop(),
                        icon: 'document',
                        badge: change === 'untracked' ? 'U' : change.slice(0, 1).toUpperCase(),
                        tone: entry.conflict ? 'error' : 'added',
                        data: entry
                    });
                    if (rows.length % 128 === 0) await astra.sleep(0, options);
                }
                return {
                    rows,
                    subtitle: model.branch,
                    empty: 'No changes',
                    update: { branch: model.branch }
                };
            }
        }
    );
    ui.registerCommand(
        { id: 'ui.gitStatus', title: 'Source control: refresh changes', shortcut: 'Ctrl+Shift+G' },
        async () => {
            await ui.update({ sidebar: 'git', sidebarVisible: true });
            await ui.refresh('git');
        }
    );
    for (const [id, method] of [
        ['ui.gitStage', 'git.stage'],
        ['ui.gitUnstage', 'git.unstage']
    ]) {
        ui.registerCommand(
            { id, title: method === 'git.stage' ? 'Stage file' : 'Unstage file' },
            async () => {
                const state = await ui.read(),
                    path = state.gitPath || state.document?.path;
                if (!path) return;
                const model = await request(method, { paths: [path] });
                await ui.renderView('git', model);
                await ui.update({
                    status: 'Git index updated',
                    dismissPrompt: true
                });
            }
        );
    }
}
