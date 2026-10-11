import { ui, request } from 'astra/ui';

let exiting,
    exitChoice,
    identity = 0;
async function closeWindow(control) {
    const options = { signal: control.signal };
    // Dismiss unrelated modal input before waiting for its cancelled workflow.
    await ui.execute('workbench.cancelDocumentSearch');
    await ui.execute('workbench.cancelWorkspaceSearch', { shutdown: true });
    await ui.native('ui.cancel');
    // A provider can be blocked in another process. Stop unfinished clipboard
    // work before the idle barrier, while retaining successfully copied text.
    await request('_ui.clipboard.cancelPending', {}, options);
    for (;;) {
        await ui.waitForIdle(options);
        const state = await ui.read({ immediate: true });
        if (state.tabs.length) {
            const page = await ui.read(
                {
                    immediate: true,
                    tabOffset: Math.max(0, state.tabs.length - 32),
                    tabLimit: 32
                },
                options
            );
            if (
                !Array.isArray(page.tabDocuments) ||
                page.tabDocuments.length > 32 ||
                page.tabDocuments.some(
                    (document) =>
                        typeof document.id !== 'string' ||
                        typeof document.revision !== 'string' ||
                        typeof document.dirty !== 'boolean'
                ) ||
                new Set(page.tabDocuments.map((document) => document.id)).size !==
                    page.tabDocuments.length
            ) {
                throw Error('Invalid bounded document page while closing the window');
            }
            const background = page.tabDocuments.filter(
                (document) => !document.dirty && document.id !== state.document?.id
            );
            if (background.length) {
                const closing = background.map(async (document) => {
                    try {
                        const result = await request(
                            'document.close',
                            {
                                id: document.id,
                                baseRevision: document.revision,
                                discardChanges: false
                            },
                            options
                        );
                        if (result.id !== document.id || result.closed !== true) {
                            throw Error('Close did not acknowledge the captured document');
                        }
                        await ui.update({ closedDocument: document.id });
                        return { id: document.id, closed: true };
                    } catch (error) {
                        if (error.code !== -32001 && error.code !== -32003) throw error;
                        // Concurrent edits keep this document for the next fenced decision.
                        return { id: document.id, closed: false, code: error.code };
                    }
                });
                const completed = await Promise.allSettled(closing);
                const failed = completed.find((result) => result.status === 'rejected');
                if (failed) throw failed.reason;
                if (completed.some((result) => !result.value.closed)) {
                    await ui.update({ status: 'Documents changed while closing; checking again' });
                }
                continue;
            }
            const id = state.tabs.at(-1);
            await ui.update({ activate: id });
            if (!(await ui.execute('workbench.close', { id, signal: control.signal })))
                return false;
            continue;
        }
        let bypass = false;
        for (;;) {
            try {
                await ui.withProgress(
                    {
                        title: 'Saving recovery checkpoint before exit...',
                        detail: 'The window will close after durable storage succeeds.',
                        cancel: 'Keep open (Esc)'
                    },
                    (context) => ui.execute('ui.checkpoint', context),
                    options
                );
                break;
            } catch (error) {
                if (error.name === 'AbortError') return false;
                await ui.update({
                    error: error.message,
                    status: 'Recovery checkpoint failed; window kept open'
                });
                const id = `exit-${++identity}`;
                exitChoice = id;
                let decision;
                try {
                    decision = await ui.choose(
                        {
                            id,
                            title: 'Recovery checkpoint failed',
                            detail: `${error.message}\nExit anyway may leave recovery state incomplete.`,
                            choices: [
                                { label: 'Retry', value: 'retry' },
                                { label: 'Keep open', value: 'cancel' },
                                { label: 'Exit anyway', value: 'exit' }
                            ],
                            keys: ['', '', 'e']
                        },
                        options
                    );
                } finally {
                    if (exitChoice === id) exitChoice = undefined;
                }
                if (!decision || decision.value === 'cancel') {
                    await ui.update({ error: '', status: 'Window kept open' });
                    return false;
                }
                if (decision.value === 'exit') {
                    bypass = true;
                    break;
                }
            }
        }
        await ui.waitForIdle(options);
        // New documents or work arriving during the checkpoint require another
        // close decision. Native teardown grants the request only when quiescent.
        if (await ui.requestClose(options)) return true;
        await ui.update({
            status: bypass
                ? 'New documents arrived; review them before exit'
                : 'New documents arrived; checking changes before exit'
        });
    }
}
export function registerLifecycle() {
    ui.registerCommand(
        { id: 'ui.closeWindow', title: 'Close window', shortcut: 'Alt+F4' },
        async () => {
            if (exiting) return false;
            const control = new AbortController();
            exiting = control;
            let suspended = true;
            try {
                suspended = await ui.chrome.suspend();
                return await closeWindow(control);
            } catch (error) {
                if (error.name === 'AbortError') return false;
                throw error;
            } finally {
                try {
                    if (!suspended) await ui.chrome.resume();
                } finally {
                    if (exiting === control) exiting = undefined;
                }
            }
        }
    );
    ui.registerCommand(
        { id: 'ui.cancel', title: 'Cancel current interaction', palette: false },
        async () => {
            exiting?.abort();
            const workspace = await ui.execute('workbench.cancelWorkspaceSearch', {
                onlyFocused: true
            });
            if (workspace) {
                await ui.update({ dismissPrompt: true, error: '' });
                return;
            }
            await ui.execute('workbench.cancelDocumentSearch');
            await ui.native('ui.cancel');
        }
    );
    for (const [id, index] of [
        ['ui.retryExit', 0],
        ['ui.exitWithoutCheckpoint', 2]
    ]) {
        ui.registerCommand(
            {
                id,
                title:
                    id === 'ui.retryExit'
                        ? 'Retry exit checkpoint'
                        : 'Confirm exit without checkpoint',
                palette: false
            },
            async () => {
                if (exitChoice) await ui.answerChoice(exitChoice, index);
            }
        );
    }
}
