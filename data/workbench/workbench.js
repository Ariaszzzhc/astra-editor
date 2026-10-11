import { registerFiles } from 'astra/files';
import { registerIcons } from 'astra/icons';
import { registerGit } from 'astra/git';
import { registerSearch } from 'astra/search';
import { registerNativeCommands } from 'astra/commands';
import { registerClipboard } from 'astra/clipboard';
import { registerLanguage } from 'astra/language';
import { registerRecovery } from 'astra/recovery';
import { registerLifecycle } from 'astra/lifecycle';
import { registerOutline } from 'astra/outline';
export function initialize() {
    registerIcons();
    registerNativeCommands();
    registerFiles();
    registerGit();
    registerSearch();
    registerClipboard();
    registerLanguage();
    registerRecovery();
    registerLifecycle();
    registerOutline();
}
