import { ui, request } from 'astra/ui';
// Labels, shortcuts and command registration are client policy. Native input
// primitives remain behind the adapter while their remaining workflows migrate.
export function registerNativeCommands() {
    const editing = [
        ['move_left', 'Move caret left', 'Left'],
        ['move_right', 'Move caret right', 'Right'],
        ['move_word_left', 'Move to previous word', 'Ctrl+Left'],
        ['move_word_right', 'Move to next word', 'Ctrl+Right'],
        ['move_up', 'Move caret up', 'Up'],
        ['move_down', 'Move caret down', 'Down'],
        ['move_home', 'Move to line start', 'Home'],
        ['move_end', 'Move to line end', 'End'],
        ['move_document_start', 'Move to document start', 'Ctrl+Home'],
        ['move_document_end', 'Move to document end', 'Ctrl+End'],
        ['select_word', 'Select word', ''],
        ['select_next_occurrence', 'Select next occurrence', 'Ctrl+D'],
        ['backspace', 'Delete left', 'Backspace'],
        ['delete_forward', 'Delete right', 'Delete'],
        ['delete_word_backward', 'Delete previous word', 'Ctrl+Backspace'],
        ['delete_word_forward', 'Delete next word', 'Ctrl+Delete'],
        ['newline', 'Insert new line', 'Enter'],
        ['indent', 'Indent lines', 'Tab'],
        ['outdent', 'Outdent lines', 'Shift+Tab'],
        ['insert_text', 'Insert text', '', false]
    ];
    for (const [name, title, shortcut, palette = true] of editing) {
        const id = `editor.${name}`;
        ui.registerCommand(
            { id, title, ...(shortcut ? { shortcut } : {}), palette },
            (parameters) => ui.native(id, parameters)
        );
    }
    const commands = [
        ['ui.closeTab', 'Close active tab', 'Ctrl+W'],
        ['editor.undo', 'Undo', 'Ctrl+Z'],
        ['editor.redo', 'Redo', 'Ctrl+Y'],
        ['editor.select_all', 'Select all', 'Ctrl+A'],
        ['editor.toggle_line_comment', 'Toggle line comment', 'Ctrl+/'],
        ['editor.duplicate_line', 'Duplicate line', 'Shift+Alt+Down'],
        ['editor.add_cursor_above', 'Add cursor above', 'Ctrl+Alt+Up'],
        ['editor.add_cursor_below', 'Add cursor below', 'Ctrl+Alt+Down'],
        ['ui.closeWindow', 'Close window', 'Alt+F4'],
        ['ui.dismissWorkspaceNotice', 'Dismiss workspace notice'],
        ['ui.selectFunction', 'Select enclosing function'],
        ['ui.selectClass', 'Select enclosing class'],
        ['ui.selectBlock', 'Select enclosing block'],
        ['ui.selectString', 'Select string content'],
        ['ui.recovery', 'Review unsaved sessions'],
        ['ui.checkpoint', 'Write recovery checkpoint']
    ];
    for (const [id, title, shortcut] of commands) {
        ui.registerCommand({ id, title, ...(shortcut ? { shortcut } : {}) }, (parameters) =>
            ui.native(id, parameters)
        );
    }
    astra.bind('Ctrl+Shift+Z', 'editor.redo');
    ui.registerCommand(
        { id: 'ui.commandPalette', title: 'Command palette', shortcut: 'Ctrl+Shift+P' },
        async () => {
            const items = (await ui.commands())
                .filter((command) => command.palette)
                .map((command) => ({
                    ...command,
                    label: command.title + (command.shortcut ? `    ${command.shortcut}` : '')
                }));
            const selected = await ui.pick({
                title: 'COMMAND PALETTE',
                placeholder: 'Type a command...',
                items
            });
            if (!selected) return;
            if (selected.source === 'client') return await ui.execute(selected.id);
            const { document } = await ui.read();
            const result = await request('script.command', {
                name: selected.id,
                params: document ? { id: document.id, baseRevision: document.revision } : {}
            });
            if (result?.id && result?.revision)
                await ui.update({ document: result, revealCaret: true });
            return result;
        }
    );
}
