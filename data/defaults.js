// Astra's complete default policy. User configuration runs afterwards and can
// merge settings, replace one binding, or replace a named command independently.
astra.configure({
    editor: {
        fontSize: 16,
        fontFamilies: ['Cascadia Code', 'Consolas', 'monospace'],
        tabSize: 4,
        insertSpaces: true,
        lineHeight: 1.4,
        cursorBlink: true
    },
    theme: {
        name: 'Astra Dark',
        background: '#1F1F1F',
        foreground: '#CCCCCC',
        sidebar: '#181818',
        activityBar: '#181818',
        tabs: '#181818',
        panel: '#252526',
        activeTab: '#1F1F1F',
        border: '#2B2B2B',
        selection: '#264F78',
        accent: '#0078D4',
        status: '#181818',
        muted: '#9D9D9D',
        statusForeground: '#CCCCCC',
        hover: '#2A2D2E',
        currentLine: '#272727',
        input: '#313131',
        cursor: '#AEAFAD',
        scrollbar: '#424242',
        folder: '#D7BA7D',
        codeIcon: '#75BEFF',
        scriptIcon: '#E5C07B',
        added: '#73C991',
        changed: '#75BEFF',
        noticeBackground: '#332D18',
        recoveryBackground: '#263222',
        errorBackground: '#3A1D1D',
        error: '#F48771',
        warning: '#CCA700',
        keyword: '#C586C0',
        string: '#CE9178',
        comment: '#6A9955',
        number: '#B5CEA8',
        type: '#4EC9B0',
        function: '#DCDCAA',
        property: '#9CDCFE',
        variable: '#9CDCFE',
        heading: '#569CD6',
        syntaxError: '#F44747'
    },
    fileIcons: {
        fallback: { glyph: 'document', tone: 'muted' },
        folder: { glyph: 'folder', tone: 'folder' },
        directories: {
            '.git': { glyph: 'branch', tone: 'added' },
            '.github': { glyph: 'branch', tone: 'muted' }
        },
        filenames: {
            'cmakelists.txt': { glyph: 'settings', tone: 'accent' },
            '.editorconfig': { glyph: 'settings', tone: 'muted' },
            '.gitignore': { glyph: 'branch', tone: 'added' },
            'package.json': { glyph: 'braces', tone: 'added' },
            'xmake.lua': { glyph: 'settings', tone: 'accent' }
        },
        extensions: {
            '.c': { glyph: 'code', tone: 'codeIcon' },
            '.cc': { glyph: 'code', tone: 'codeIcon' },
            '.cpp': { glyph: 'code', tone: 'codeIcon' },
            '.cxx': { glyph: 'code', tone: 'codeIcon' },
            '.h': { glyph: 'code', tone: 'codeIcon' },
            '.hh': { glyph: 'code', tone: 'codeIcon' },
            '.hpp': { glyph: 'code', tone: 'codeIcon' },
            '.hxx': { glyph: 'code', tone: 'codeIcon' },
            '.js': { glyph: 'braces', tone: 'scriptIcon' },
            '.jsx': { glyph: 'braces', tone: 'scriptIcon' },
            '.mjs': { glyph: 'braces', tone: 'scriptIcon' },
            '.cjs': { glyph: 'braces', tone: 'scriptIcon' },
            '.ts': { glyph: 'braces', tone: 'codeIcon' },
            '.tsx': { glyph: 'braces', tone: 'codeIcon' },
            '.json': { glyph: 'braces', tone: 'scriptIcon' },
            '.jsonc': { glyph: 'braces', tone: 'scriptIcon' },
            '.py': { glyph: 'code', tone: 'scriptIcon' },
            '.pyi': { glyph: 'code', tone: 'scriptIcon' },
            '.lua': { glyph: 'code', tone: 'accent' },
            '.rs': { glyph: 'code', tone: 'changed' },
            '.go': { glyph: 'code', tone: 'codeIcon' },
            '.wgsl': { glyph: 'code', tone: 'accent' },
            '.md': { glyph: 'document', tone: 'codeIcon' },
            '.markdown': { glyph: 'document', tone: 'codeIcon' },
            '.yaml': { glyph: 'settings', tone: 'warning' },
            '.yml': { glyph: 'settings', tone: 'warning' },
            '.toml': { glyph: 'settings', tone: 'muted' },
            '.ini': { glyph: 'settings', tone: 'muted' },
            '.sh': { glyph: 'code', tone: 'added' },
            '.ps1': { glyph: 'code', tone: 'accent' }
        }
    },
    workbench: {
        sidebarWidth: 248,
        uiFontSize: 13,
        uiFontFamilies: ['Segoe UI', 'Microsoft YaHei UI', 'Segoe UI Emoji'],
        uiMonospaceFontFamilies: [
            'Cascadia Mono',
            'Consolas',
            'Microsoft YaHei UI',
            'Segoe UI Emoji',
            'monospace'
        ]
    },
    git: { inheritGlobalConfig: true },
    languageServers: {
        c: { command: 'clangd', arguments: ['--background-index'] },
        cpp: { command: 'clangd', arguments: ['--background-index'] },
        javascript: { command: 'typescript-language-server', arguments: ['--stdio'] },
        typescript: { command: 'typescript-language-server', arguments: ['--stdio'] },
        python: { command: 'basedpyright-langserver', arguments: ['--stdio'] }
    }
});

for (const command of [
    'move_left',
    'move_right',
    'move_word_left',
    'move_word_right',
    'move_up',
    'move_down',
    'move_home',
    'move_end',
    'move_document_start',
    'move_document_end',
    'select_all',
    'select_word',
    'select_next_occurrence',
    'backspace',
    'delete_forward',
    'delete_word_backward',
    'delete_word_forward',
    'newline',
    'indent',
    'outdent',
    'toggle_line_comment',
    'duplicate_line',
    'add_cursor_above',
    'add_cursor_below',
    'insert_text'
]) {
    astra.registerCommand(`editor.${command}`, (parameters) => {
        return astra.request('document.command', {
            ...parameters,
            command
        });
    });
}
astra.registerCommand('editor.undo', (p) => astra.request('document.undo', p));
astra.registerCommand('editor.redo', (p) => astra.request('document.redo', p));

const bindings = {
    Left: 'editor.move_left',
    Right: 'editor.move_right',
    Up: 'editor.move_up',
    Down: 'editor.move_down',
    'Ctrl+Left': 'editor.move_word_left',
    'Ctrl+Right': 'editor.move_word_right',
    Home: 'editor.move_home',
    End: 'editor.move_end',
    'Ctrl+Home': 'editor.move_document_start',
    'Ctrl+End': 'editor.move_document_end',
    Backspace: 'editor.backspace',
    Delete: 'editor.delete_forward',
    'Ctrl+Backspace': 'editor.delete_word_backward',
    'Ctrl+Delete': 'editor.delete_word_forward',
    Enter: 'editor.newline',
    Tab: 'editor.indent',
    'Shift+Tab': 'editor.outdent',
    'Ctrl+A': 'editor.select_all',
    'Ctrl+Z': 'editor.undo',
    'Ctrl+Y': 'editor.redo',
    'Ctrl+D': 'editor.select_next_occurrence',
    'Ctrl+Shift+Z': 'editor.redo',
    'Ctrl+/': 'editor.toggle_line_comment',
    'Ctrl+Alt+Up': 'editor.add_cursor_above',
    'Ctrl+Alt+Down': 'editor.add_cursor_below',
    'Alt+Shift+Down': 'editor.duplicate_line',
    'Ctrl+N': 'ui.new',
    'Ctrl+S': 'ui.save',
    'Ctrl+Shift+S': 'ui.saveAs',
    'Ctrl+W': 'ui.closeTab',
    'Ctrl+P': 'ui.quickOpen',
    'Ctrl+Shift+P': 'ui.commandPalette',
    F1: 'ui.commandPalette',
    'Ctrl+C': 'ui.copy',
    'Ctrl+X': 'ui.cut',
    'Ctrl+V': 'ui.paste',
    'Ctrl+F': 'ui.find',
    'Ctrl+H': 'ui.replace',
    'Ctrl+Shift+F': 'ui.search',
    'Ctrl+Shift+H': 'ui.workspaceReplace',
    'Ctrl+Tab': 'ui.nextTab',
    'Ctrl+Shift+Tab': 'ui.previousTab',
    'Ctrl+PageDown': 'ui.nextTab',
    'Ctrl+PageUp': 'ui.previousTab',
    PageUp: 'ui.pageUp',
    PageDown: 'ui.pageDown'
};
for (const [key, command] of Object.entries(bindings)) astra.bind(key, command);
// Selection extension is interpreted by the UI for movement commands, preserving
// a user's normal motion bindings when Shift is pressed.
