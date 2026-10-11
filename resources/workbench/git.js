import { ui, request } from 'astra/ui';
export function registerGit() {
    let diffController;
    let activeDiff;
    const closeComparison = async (target) => {
        const session = activeDiff;
        if (!session || session.target !== target) {
            return;
        }
        session.controller.abort();
        if (session.presentation) {
            await Promise.allSettled([session.presentation]);
        }
    };
    const lineTones = async (text, signal) => {
        const ranges = [];
        let row = 0;
        for (const match of text.matchAll(/[^\r\n]*(?:\r\n|\r|\n|$)/g)) {
            const line = match[0];
            let tone = 'normal';
            if (line.startsWith('+') && !line.startsWith('+++')) {
                tone = 'added';
            } else if (line.startsWith('-') && !line.startsWith('---')) {
                tone = 'removed';
            } else if (line.startsWith('@@')) {
                tone = 'accent';
            }
            if (tone !== 'normal') {
                const previous = ranges.at(-1);
                if (previous?.end === row && previous.tone === tone) {
                    previous.end++;
                } else {
                    ranges.push({ begin: row, end: row + 1, tone });
                }
            }
            if (++row % 512 === 0) {
                await astra.sleep(0, { signal });
            }
        }
        return ranges;
    };
    ui.registerCommand(
        { id: 'workbench.gitDiff', title: 'Open source-control comparison' },
        async ({ path, staged = false }) => {
            if (!path) {
                return;
            }
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
                if (diffController !== controller || controller.signal.aborted) {
                    return;
                }
                const target = { path, staged, previewId: model.previewId };
                const session = { target, controller };
                activeDiff = session;
                if (!staged) {
                    const state = await ui.read({}, { signal: controller.signal });
                    for (const id of state.tabs || []) {
                        const { document } = await ui.read({ id }, { signal: controller.signal });
                        if (document?.path === path && !document.dirty) {
                            await ui.update({
                                gutter: {
                                    id,
                                    revision: document.revision,
                                    marks: model.gutter || []
                                }
                            });
                        }
                    }
                }
                if (diffController !== controller || controller.signal.aborted) {
                    return;
                }
                const actions = [
                    { id: 'ui.gitStage', label: 'Stage', key: 's' },
                    { id: 'ui.gitUnstage', label: 'Unstage', key: 'u' }
                ];
                if (!staged && target.previewId) {
                    actions.push({
                        id: 'ui.gitDiscard',
                        label: 'Discard changes…',
                        key: 'd',
                        destructive: true
                    });
                }
                const tones = await lineTones(text, controller.signal);
                if (diffController !== controller || controller.signal.aborted) {
                    return;
                }
                session.presentation = ui.showText(
                    {
                        title: `DIFF — ${path} [${staged ? 'staged' : 'working tree'}] — Esc to close`,
                        text,
                        monospace: true,
                        actions,
                        lineTones: tones
                    },
                    { signal: controller.signal }
                );
                const action = await session.presentation;
                if (activeDiff === session) {
                    activeDiff = undefined;
                }
                if (action && actions.some((candidate) => candidate.id === action)) {
                    await ui.execute(action, { target });
                }
            } finally {
                if (diffController === controller) {
                    diffController = undefined;
                    activeDiff = undefined;
                }
            }
        }
    );
    ui.registerCommand({ id: 'ui.gitDiff', title: 'Show file diff' }, async () => {
        const { document } = await ui.read();
        if (document?.path) {
            await ui.execute('workbench.gitDiff', { path: document.path });
        }
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
        if (!message) {
            return;
        }
        await request('git.commit', { message });
        await ui.refresh('git');
        await ui.update({ status: 'Commit created' });
    });
    ui.registerCommand(
        { id: 'ui.gitDiscard', title: 'Discard previewed working-tree changes...' },
        async ({ target = activeDiff?.target } = {}) => {
            if (!target?.path || target.staged || !target.previewId) {
                return;
            }
            const { path, previewId } = target;
            await closeComparison(target);
            if (
                !(await ui.confirm({
                    title: 'Discard the previewed working tree change?',
                    detail: `${path}\nThis removes changes from disk. It cannot be undone here.`,
                    accept: 'Discard changes'
                }))
            ) {
                return;
            }
            const model = await request('git.discard', { previewId });
            await ui.renderView('git', model);
            await ui.refresh('explorer');
            await ui.update({ status: 'Working tree change discarded' });
        }
    );
    ui.registerView(
        { id: 'git', title: 'Source Control', icon: 'branch', order: 2 },
        (_, options) => ({ pages: astra.operationPages('git.status', {}, options) }),
        {
            show: () => ui.execute('ui.gitStatus'),
            activate: (entry) =>
                ui.execute('workbench.gitDiff', {
                    path: entry.path,
                    staged: entry.unstaged === 'unchanged'
                }),
            project(model, options) {
                let branch = model.branch ?? '';
                async function* rows() {
                    for await (const page of model.pages ?? [model]) {
                        options.signal?.throwIfAborted();
                        if (page.cancelled) {
                            throw Object.assign(new Error('Git status cancelled'), {
                                name: 'AbortError'
                            });
                        }
                        branch = page.branch ?? branch;
                        for (const entry of page.entries) {
                            const change =
                                entry.unstaged !== 'unchanged' ? entry.unstaged : entry.staged;
                            yield {
                                id: entry.path,
                                label: entry.path.split(/[\\/]/).pop(),
                                icon: 'document',
                                badge:
                                    change === 'untracked' ? 'U' : change.slice(0, 1).toUpperCase(),
                                tone: entry.conflict ? 'error' : 'added',
                                data: entry
                            };
                        }
                    }
                }
                return {
                    rows: rows(),
                    empty: 'No changes',
                    complete: () => ({ subtitle: branch, update: { branch } })
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
            async ({ target = activeDiff?.target } = {}) => {
                const path = target?.path || (await ui.read()).document?.path;
                if (!path) {
                    return;
                }
                await closeComparison(target);
                const model = await request(method, { paths: [path] });
                await ui.renderView('git', model);
                await ui.update({ status: 'Git index updated' });
            }
        );
    }
}
