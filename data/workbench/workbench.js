import { registerFiles } from 'astra/files';
import { registerGit } from 'astra/git';
import { registerSearch } from 'astra/search';
import { registerNativeCommands } from 'astra/commands';
import { registerClipboard } from 'astra/clipboard';
import { registerLanguage } from 'astra/language';
import { registerRecovery } from 'astra/recovery';
import { registerLifecycle } from 'astra/lifecycle';
export function initialize() {
    registerNativeCommands();
    registerFiles();
    registerGit();
    registerSearch();
    registerClipboard();
    registerLanguage();
    registerRecovery();
    registerLifecycle();
}
