import { ui, request } from 'astra/ui';

let discovery,
    confirmation,
    review,
    identity = 0,
    publication = 0;
const abandoned = (sessions) =>
    sessions.filter(
        (session) =>
            !session.active && ((session.buffers || []).length || (session.issues || []).length)
    );
const bufferLabel = (buffer) => {
    const name = buffer.path?.split(/[\\/]/).at(-1) || 'Untitled';
    return `${name} (${buffer.bytes || 0} bytes)${buffer.path && buffer.path !== name ? ` — ${buffer.path}` : ''}`;
};
async function publish(sessions, controller, version = publication) {
    const choices = abandoned(sessions);
    if (
        version === publication &&
        (!controller || (discovery === controller && !controller.signal.aborted))
    ) {
        await ui.update({ recoveryAvailable: choices.length > 0 });
    }
    return choices;
}
async function scan() {
    discovery?.abort();
    const controller = new AbortController();
    discovery = controller;
    const options = { signal: controller.signal },
        deadline = Date.now() + 30000,
        version = publication;
    try {
        for (;;) {
            const state = await request('recovery.status', {}, options);
            if (state.error) throw new Error(`Recovery: ${state.error}`);
            if (!state.enabled) return await publish([], controller, version);
            if (state.ready) break;
            if (Date.now() >= deadline)
                throw new Error('Recovery initialization timed out; review sessions to retry');
            await astra.sleep(500, options);
        }
        const result = await request('recovery.list', {}, options);
        return await publish(result.sessions || [], controller, version);
    } catch (error) {
        if (error.name === 'AbortError') return null;
        throw error;
    } finally {
        if (discovery === controller) discovery = undefined;
    }
}
async function reviewSessions() {
    const sessions = await scan();
    if (!sessions) return;
    if (!sessions.length) {
        await ui.update({ status: 'No abandoned unsaved sessions found' });
        return;
    }
    const choice = await ui.pick({
        title: 'RECOVERY — SELECT A SESSION TO REVIEW',
        placeholder: 'Filter by session or file path',
        items: sessions.map((session) => ({
            session,
            label: `${session.id} — ${(session.buffers || []).length} unsaved buffers  ${(session.buffers || []).map((buffer) => buffer.path || 'Untitled').join(' · ')}`
        }))
    });
    if (!choice) return;
    const session = choice.session,
        buffers = session.buffers || [],
        issues = session.issues || [];
    const rows = [
        ...buffers.map(bufferLabel),
        ...issues.map((issue) => `Issue: ${issue.path || ''} — ${issue.message || ''}`)
    ];
    if (!buffers.length) {
        await ui.showText({
            title: 'RECOVERY ISSUES — NO RECOVERABLE BUFFERS',
            text: rows.join('\n')
        });
        return;
    }
    const id = `recovery-${++identity}`;
    confirmation = id;
    let accepted;
    try {
        accepted = await ui.confirm({
            id,
            title: 'Restore unsaved buffers from this session?',
            accept: 'Restore session',
            detail: 'Recovered buffers stay unsaved. Review and save them afterward.',
            items: rows
        });
    } finally {
        if (confirmation === id) confirmation = undefined;
    }
    if (!accepted) return;
    const result = await request('recovery.restore', { sessionId: session.id });
    await ui.update({
        documents: result.documents,
        ...(result.documents.length ? { activate: result.documents[0].id } : {}),
        status: 'Recovered buffers are unsaved; review and save to persist',
        error: ''
    });
    const listed = await request('recovery.list');
    await publish(listed.sessions || []);
}
export function registerRecovery() {
    ui.registerCommand(
        { id: 'workbench.recoveryScan', title: 'Discover recovery sessions', palette: false },
        scan
    );
    ui.registerCommand({ id: 'ui.recovery', title: 'Review unsaved sessions' }, async () => {
        // Repeated invocations cannot compete for the one native modal surface.
        if (review) return;
        review = true;
        try {
            return await reviewSessions();
        } finally {
            review = false;
        }
    });
    ui.registerCommand(
        { id: 'ui.confirmRecovery', title: 'Confirm recovery', palette: false },
        async () => {
            if (confirmation) await ui.answerConfirm(confirmation);
        }
    );
    ui.registerCommand(
        { id: 'ui.checkpoint', title: 'Write recovery checkpoint' },
        async (parameters = {}) => {
            await ui.update({ status: 'Writing recovery checkpoint...', error: '' });
            const result = await request('recovery.flush', {}, { signal: parameters.signal });
            await ui.update({
                status:
                    result.enabled === false ? 'Recovery is disabled' : 'Recovery checkpoint saved',
                error: ''
            });
            return result;
        }
    );
    astra.on('recovery.available', async (params) => {
        ++publication;
        await publish(params.sessions || []);
    });
    astra.on('recovery.error', (params) =>
        ui.update({
            error: `Recovery checkpoint: ${params.message || 'Storage failed'}`,
            status: 'Recovery needs attention'
        })
    );
    astra.on('recovery.warning', (params) =>
        ui.update({
            error: (params.issues || [])
                .map((issue) => `${issue.path || ''}: ${issue.message || ''}`)
                .join('\n'),
            status: 'Recovery maintenance needs attention'
        })
    );
}
