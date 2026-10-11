import { ui } from 'astra/ui';

export function registerChrome() {
    ui.registerChrome(present, { activate });
}

function caption(value, limit = 512, byteLimit = Infinity) {
    let result = '',
        count = 0,
        bytes = 0;
    for (const scalar of String(value ?? '')) {
        if (count++ === limit) break;
        const text = /[\r\n\0]/.test(scalar) ? ' ' : scalar;
        const size = astra.utf8ByteLength(text);
        if (bytes + size > byteLimit) break;
        bytes += size;
        result += text;
    }
    return result;
}

function present(context) {
    const shortcut = (name) =>
        caption(astra.settings().workbench?.commands?.[name]?.shortcut, 32, 64);
    const command = (id, label, name, icon) => ({
        id,
        label,
        ...(icon ? { icon } : {}),
        data: { action: 'command', name }
    });
    const model = {
        brand: 'Astra',
        hint: shortcut('ui.commandPalette'),
        command: [
            command(
                'commands',
                caption(context.workspace, 128) || 'Workspace',
                'ui.commandPalette',
                'search'
            )
        ],
        activity: context.views.map((view) => ({
            id: view.id,
            label: view.title,
            icon: view.icon,
            selected: view.id === context.activeView && context.sidebarVisible,
            data: { action: 'view', id: view.id }
        })),
        manage: [command('manage', 'Manage commands', 'ui.commandPalette', 'settings')],
        breadcrumb: [],
        statusLeft: [
            command('branch', caption(context.branch, 96) || 'Astra', 'ui.gitStatus', 'branch')
        ],
        statusRight: [],
        welcomeTitle: [{ id: 'title', label: 'Astra' }],
        welcomeDetail: [{ id: 'detail', label: 'A focused place to write code.' }],
        welcomeActions: [
            ['new', 'New file', 'ui.new', 'document'],
            ['open', 'Open file', 'ui.quickOpen', 'search'],
            ['commands', 'Commands', 'ui.commandPalette', 'settings']
        ].map(([id, label, name, icon]) => ({
            ...command(id, label, name, icon),
            badge: shortcut(name)
        })),
        notices: []
    };
    if (context.recoveryAvailable)
        model.notices.push({
            ...command(
                'recovery',
                'Unsaved recovery sessions are available',
                'ui.recovery',
                'document'
            ),
            tone: 'added',
            badge: 'Review'
        });
    const notice = caption(context.notice);
    if (notice)
        model.notices.push({
            ...command('workspace', notice, 'ui.dismissWorkspaceNotice', 'warning'),
            tone: 'changed',
            badge: 'Dismiss'
        });
    const message = caption(context.message);
    if (message) model.statusLeft.push({ id: 'message', label: message });
    const document = context.document;
    if (!document) return model;
    const parts = document.path.replaceAll('\\', '/').split('/').filter(Boolean);
    let bytes = 0;
    for (let index = parts.length - 1; index >= 0 && model.breadcrumb.length < 32; --index) {
        const row = {
            id: `path-${index}`,
            label: caption(parts[index]),
            icon: index === parts.length - 1 ? 'document' : 'folder',
            data: { action: 'reveal', path: parts.slice(0, index + 1).join('/') }
        };
        const size = astra.utf8ByteLength(JSON.stringify(row));
        if (bytes + size > 16384) break;
        bytes += size;
        model.breadcrumb.unshift(row);
    }
    if (!parts.length)
        model.breadcrumb.push({ id: 'untitled', label: 'Untitled', icon: 'document' });
    const caret = document.caret;
    const visual = caret.visualColumn !== undefined;
    model.statusRight.push({
        id: 'caret',
        label: `Ln ${caret.line + 1}, ${visual ? 'Col' : 'UTF16 Col'} ${(visual ? caret.visualColumn : caret.utf16Column) + 1}`
    });
    model.statusRight.push({ id: 'encoding', label: caption(document.encoding, 32) });
    model.statusRight.push({
        id: 'indent',
        label: `${document.insertSpaces ? 'Spaces' : 'Tab Size'}: ${document.tabSize}`
    });
    if (document.languageActive) model.statusRight.push({ id: 'language', label: 'LSP' });
    return model;
}

async function activate(data, row, context) {
    if (data?.action === 'command') {
        const parameters =
            data.name === 'ui.dismissWorkspaceNotice' && typeof context.noticeSequence === 'string'
                ? { noticeSequence: context.noticeSequence }
                : {};
        return await ui.execute(data.name, parameters);
    }
    if (data?.action === 'view') return await ui.showView(data.id);
    if (data?.action === 'reveal') {
        await ui.execute('ui.explorer');
        const view = await ui.views.read('explorer');
        return await ui.views.reveal('explorer', data.path, {
            activity: context.activity,
            version: view.version
        });
    }
    return null;
}
