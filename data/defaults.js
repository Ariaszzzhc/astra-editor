// Astra's complete default policy. User configuration runs afterwards and can
// merge settings, replace one binding, or replace a named command independently.
astra.configure({
    editor: {
        fontSize: 16, fontFamilies: ["Cascadia Code", "Consolas", "monospace"],
        tabSize: 4, insertSpaces: true,
        lineHeight: 1.4, cursorBlink: true
    },
    theme: {
        name: "Astra Dark", background: "#1E1E1E", foreground: "#D4D4D4",
        sidebar: "#252526", activityBar: "#333333", tabs: "#2D2D2D",
        activeTab: "#1E1E1E", border: "#3C3C3C", selection: "#264F78",
        accent: "#007ACC", status: "#007ACC", muted: "#858585",
        error: "#F48771", warning: "#CCA700", keyword: "#C586C0",
        string: "#CE9178", comment: "#6A9955", number: "#B5CEA8",
        type: "#4EC9B0", function: "#DCDCAA"
    },
    git: {inheritGlobalConfig: true},
    languageServers: {
        c: {command: "clangd", arguments: ["--background-index"]},
        cpp: {command: "clangd", arguments: ["--background-index"]},
        javascript: {command: "typescript-language-server", arguments: ["--stdio"]},
        typescript: {command: "typescript-language-server", arguments: ["--stdio"]},
        python: {command: "basedpyright-langserver", arguments: ["--stdio"]}
    }
});

for (const command of [
    "move_left", "move_right", "move_word_left", "move_word_right", "move_up", "move_down",
    "move_home", "move_end", "move_document_start", "move_document_end", "select_all",
    "select_word", "select_next_occurrence", "backspace", "delete_forward", "delete_word_backward", "delete_word_forward",
    "newline", "indent", "outdent", "toggle_line_comment", "duplicate_line", "add_cursor_above",
    "add_cursor_below", "insert_text"
]) {
    astra.registerCommand(`editor.${command}`, (parameters) => {
        return astra.request("document.command", {
            ...parameters, command
        });
    });
}
astra.registerCommand("editor.undo", (p) => astra.request("document.undo", p));
astra.registerCommand("editor.redo", (p) => astra.request("document.redo", p));

const bindings = {
    "Left": "editor.move_left", "Right": "editor.move_right",
    "Up": "editor.move_up", "Down": "editor.move_down",
    "Ctrl+Left": "editor.move_word_left", "Ctrl+Right": "editor.move_word_right",
    "Home": "editor.move_home", "End": "editor.move_end",
    "Ctrl+Home": "editor.move_document_start", "Ctrl+End": "editor.move_document_end",
    "Backspace": "editor.backspace", "Delete": "editor.delete_forward",
    "Ctrl+Backspace": "editor.delete_word_backward", "Ctrl+Delete": "editor.delete_word_forward",
    "Enter": "editor.newline", "Tab": "editor.indent", "Shift+Tab": "editor.outdent",
    "Ctrl+A": "editor.select_all", "Ctrl+Z": "editor.undo", "Ctrl+Y": "editor.redo",
    "Ctrl+D": "editor.select_next_occurrence",
    "Ctrl+Shift+Z": "editor.redo", "Ctrl+/": "editor.toggle_line_comment",
    "Ctrl+Alt+Up": "editor.add_cursor_above", "Ctrl+Alt+Down": "editor.add_cursor_below",
    "Alt+Shift+Down": "editor.duplicate_line", "Ctrl+N": "ui.new", "Ctrl+S": "ui.save",
    "Ctrl+Shift+S": "ui.saveAs", "Ctrl+W": "ui.closeTab", "Ctrl+P": "ui.quickOpen",
    "Ctrl+Shift+P": "ui.commandPalette", "F1": "ui.commandPalette",
    "Ctrl+C": "ui.copy", "Ctrl+X": "ui.cut", "Ctrl+V": "ui.paste",
    "Ctrl+F": "ui.find", "Ctrl+H": "ui.replace", "Ctrl+Shift+F": "ui.search",
    "Ctrl+Shift+H": "ui.workspaceReplace",
    "Ctrl+Tab": "ui.nextTab", "Ctrl+Shift+Tab": "ui.previousTab"
};
for (const [key, command] of Object.entries(bindings)) astra.bind(key, command);
// Selection extension is interpreted by the UI for movement commands, preserving
// a user's normal motion bindings when Shift is pressed.
