import {ui} from 'astra/ui';
// Labels, shortcuts and command registration are client policy. Native input
// primitives remain behind the adapter while their remaining workflows migrate.
export function registerNativeCommands() {
    const commands = [
        ['ui.quickOpen','Open file by path','Ctrl+P'],
        ['ui.commandPalette','Command palette','Ctrl+Shift+P'],
        ['ui.saveAs','Save file as...','Ctrl+Shift+S'], ['ui.closeTab','Close active tab','Ctrl+W'],
        ['editor.undo','Undo','Ctrl+Z'], ['editor.redo','Redo','Ctrl+Y'],
        ['editor.select_all','Select all','Ctrl+A'], ['editor.toggle_line_comment','Toggle line comment','Ctrl+/'],
        ['editor.duplicate_line','Duplicate line','Shift+Alt+Down'],
        ['editor.add_cursor_above','Add cursor above','Ctrl+Alt+Up'], ['editor.add_cursor_below','Add cursor below','Ctrl+Alt+Down'],
        ['ui.closeWindow','Close window','Alt+F4'], ['ui.find','Find in document','Ctrl+F'],
        ['ui.replace','Replace in document','Ctrl+H'], ['ui.search','Search workspace and unsaved buffers','Ctrl+Shift+F'],
        ['ui.workspaceReplace','Replace across workspace...','Ctrl+Shift+H'],
        ['ui.reload','Reload active file from disk...'], ['ui.dismissWorkspaceNotice','Dismiss workspace notice'],
        ['ui.toggleSearchCase','Search: toggle case sensitivity','Alt+C'],
        ['ui.toggleSearchRegex','Search: toggle regular expression','Alt+R'],
        ['ui.toggleSearchWholeWord','Search: toggle whole word','Alt+W'],
        ['ui.selectFunction','Select enclosing function'], ['ui.selectClass','Select enclosing class'],
        ['ui.selectBlock','Select enclosing block'], ['ui.selectString','Select string content'],
        ['ui.completion','Trigger completion','Ctrl+Space'], ['ui.hover','Show hover information','Ctrl+K'],
        ['ui.diagnostics','Show document diagnostics','Ctrl+Shift+M'], ['ui.definition','Go to definition','F12'],
        ['ui.references','Find all references','Shift+F12'], ['ui.rename','Rename symbol','F2'],
        ['ui.format','Format document','Shift+Alt+F'], ['ui.gitDiff','Show file diff'],
        ['ui.gitCommit','Commit staged changes'], ['ui.recovery','Review unsaved sessions'],
        ['ui.checkpoint','Write recovery checkpoint']
    ];
    for (const [id, title, shortcut] of commands) {
        ui.registerCommand({id, title, ...(shortcut ? {shortcut} : {})}, () => ui.native(id));
    }
}
