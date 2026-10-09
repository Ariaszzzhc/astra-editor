import {registerFiles} from 'astra/files';
import {registerGit} from 'astra/git';
import {registerSearch} from 'astra/search';
import {registerNativeCommands} from 'astra/commands';
import {registerClipboard} from 'astra/clipboard';
export function initialize() {
    registerNativeCommands(); registerFiles(); registerGit(); registerSearch(); registerClipboard();
}
