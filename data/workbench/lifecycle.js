import {ui} from 'astra/ui';

let exiting, exitChoice, identity=0;
async function closeWindow(control) {
    const options={signal:control.signal};
    // Dismiss unrelated modal input before waiting for its cancelled workflow.
    await ui.native('ui.cancel');
    for(;;) {
        await ui.waitForIdle(options);
        const state=await ui.read({immediate:true});
        if(state.tabs.length) {
            const id=state.tabs.at(-1);
            await ui.update({activate:id});
            if(!await ui.execute('workbench.close',{id,signal:control.signal}))return false;
            continue;
        }
        let bypass=false;
        for(;;) {
            try {
                await ui.withProgress({title:'Saving recovery checkpoint before exit...',
                    detail:'The window will close after durable storage succeeds.',cancel:'Keep open (Esc)'},
                    context=>ui.execute('ui.checkpoint',context),options);
                break;
            } catch(error) {
                if(error.name==='AbortError')return false;
                await ui.update({error:error.message,status:'Recovery checkpoint failed; window kept open'});
                const id=`exit-${++identity}`;exitChoice=id;
                let decision;
                try { decision=await ui.choose({id,title:'Recovery checkpoint failed',
                    detail:`${error.message}\nExit anyway may leave recovery state incomplete.`,
                    choices:[{label:'Retry',value:'retry'},{label:'Keep open',value:'cancel'},{label:'Exit anyway',value:'exit'}],keys:['','','e']},options); }
                finally { if(exitChoice===id)exitChoice=undefined; }
                if(!decision || decision.value==='cancel') {
                    await ui.update({error:'',status:'Window kept open'});return false;
                }
                if(decision.value==='exit'){bypass=true;break;}
            }
        }
        await ui.waitForIdle(options);
        // New documents or work arriving during the checkpoint require another
        // close decision. Native teardown grants the request only when quiescent.
        if(await ui.requestClose(options))return true;
        await ui.update({status:bypass ? 'New documents arrived; review them before exit' : 'New documents arrived; checking changes before exit'});
    }
}
export function registerLifecycle() {
    ui.registerCommand({id:'ui.closeWindow',title:'Close window',shortcut:'Alt+F4'},async () => {
        if(exiting)return false;
        const control=new AbortController();exiting=control;
        try { return await closeWindow(control); }
        catch(error) { if(error.name==='AbortError')return false;throw error; }
        finally { if(exiting===control)exiting=undefined; }
    });
    ui.registerCommand({id:'ui.cancel',title:'Cancel current interaction',palette:false},async () => {
        exiting?.abort();
        await ui.native('ui.cancel');
    });
    for(const [id,index] of [['ui.retryExit',0],['ui.exitWithoutCheckpoint',2]]) {
        ui.registerCommand({id,title:id==='ui.retryExit' ? 'Retry exit checkpoint' : 'Confirm exit without checkpoint',palette:false},async () => {
            if(exitChoice)await ui.answerChoice(exitChoice,index);
        });
    }
}
