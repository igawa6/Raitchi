package com.lasrecomp.android;

import android.app.*;
import android.content.*;
import android.database.Cursor;
import android.graphics.Color;
import android.graphics.Typeface;
import android.graphics.drawable.GradientDrawable;
import android.net.Uri;
import android.os.*;
import android.provider.DocumentsContract;
import android.provider.OpenableColumns;
import android.view.*;
import android.widget.*;
import java.io.*;
import java.nio.channels.FileChannel;
import java.security.MessageDigest;
import java.util.*;
import org.json.JSONObject;

/** Raitchi library stays independent of the compiler and fresh game processes. */
public final class SharedLauncherActivity extends Activity {
    private final Handler handler=new Handler(Looper.getMainLooper());
    private final Map<String,TextView> statuses=new HashMap<>();
    private static volatile boolean copying;
    private static volatile String operationMessage="";
    private RaitchiUi.Palette colors;
    private LinearLayout page;
    private TextView message,summary,detailStatus,detailProgress;
    private Button launchButton,compileButton,importButton,cancelButton,endButton;
    private ProgressBar progressBar;
    private String detailTitle,importing;
    private Uri packageUri;
    private boolean listMode;
    private long nativeRevision=-1;
    private static boolean busy(){return copying||NativePackage.isRunning();}
    private int endingPid;
    private long endingRequestedAt;
    private boolean shortLandscape(){return getResources().getConfiguration().screenHeightDp<520;}
    private int libraryArtHeight(){return Math.max(76,Math.min(110,getResources().getConfiguration().screenHeightDp-320));}
    private final Runnable refresh=new Runnable(){public void run(){update();handler.postDelayed(this,800);}};
    private int dp(float n){return RaitchiUi.dp(this,n);}
    private TextView text(String s,float size){return RaitchiUi.text(this,s,size,colors);}
    private Button button(String s,boolean primary,View.OnClickListener click){return RaitchiUi.button(this,s,primary,colors,click);}
    private LinearLayout column(){LinearLayout v=new LinearLayout(this);v.setOrientation(LinearLayout.VERTICAL);return v;}
    private LinearLayout row(){LinearLayout v=new LinearLayout(this);v.setGravity(Gravity.CENTER_VERTICAL);return v;}
    private void gap(LinearLayout parent,int height){View v=new View(this);parent.addView(v,new LinearLayout.LayoutParams(1,dp(height)));}
    private LinearLayout.LayoutParams wrap(){return new LinearLayout.LayoutParams(-1,-2);}
    private void note(String value){operationMessage=value;if(message!=null)message.setText(value);}
    private Context dialogContext(){return new ContextThemeWrapper(this,colors.dark?android.R.style.Theme_Material_Dialog_Alert:android.R.style.Theme_Material_Light_Dialog_Alert);}
    @Override public void onCreate(Bundle state){
        super.onCreate(state);colors=RaitchiUi.palette(this);
        getWindow().setStatusBarColor(colors.background);getWindow().setNavigationBarColor(colors.background);
        getWindow().getDecorView().setSystemUiVisibility(colors.dark?0:View.SYSTEM_UI_FLAG_LIGHT_STATUS_BAR|View.SYSTEM_UI_FLAG_LIGHT_NAVIGATION_BAR);
        if(getActionBar()!=null)getActionBar().hide();
        listMode=getSharedPreferences("raitchi_library",MODE_PRIVATE).getBoolean("list",false);
        if(state!=null){detailTitle=state.getString("detail");importing=state.getString("importing");endingPid=state.getInt("ending_pid",0);endingRequestedAt=state.getLong("ending_at",0);String saved=state.getString("package");if(saved!=null)packageUri=Uri.parse(saved);}
        render();
        final Context app=getApplicationContext();
        new Thread(()->RaitchiUiPackageStaging.cleanup(app.getFilesDir(),
            ()->!busy()&&gamePid(app)==0&&processPid(app,":build")==0),"raitchi-import-cleanup").start();
        if(Build.VERSION.SDK_INT>=33&&checkSelfPermission(android.Manifest.permission.POST_NOTIFICATIONS)!=android.content.pm.PackageManager.PERMISSION_GRANTED)
            requestPermissions(new String[]{android.Manifest.permission.POST_NOTIFICATIONS},73);
    }
    @Override protected void onSaveInstanceState(Bundle state){super.onSaveInstanceState(state);state.putString("detail",detailTitle);state.putString("importing",importing);state.putInt("ending_pid",endingPid);state.putLong("ending_at",endingRequestedAt);if(packageUri!=null)state.putString("package",packageUri.toString());}
    @Override protected void onResume(){super.onResume();if(colors.dark!=RaitchiUi.palette(this).dark){colors=RaitchiUi.palette(this);render();}handler.removeCallbacks(refresh);handler.post(refresh);}
    @Override protected void onPause(){handler.removeCallbacks(refresh);super.onPause();}
    @Override public void onBackPressed(){if(detailTitle!=null){detailTitle=null;render();}else super.onBackPressed();}
    private void render(){
        statuses.clear();launchButton=compileButton=importButton=cancelButton=endButton=null;detailStatus=detailProgress=null;progressBar=null;
        LinearLayout root=column();root.setBackgroundColor(colors.background);root.setPadding(dp(24),dp(shortLandscape()?8:14),dp(24),dp(8));setContentView(root);
        LinearLayout toolbar=row();root.addView(toolbar,wrap());
        int logo=getResources().getIdentifier("raitchi_mark","drawable",getPackageName());
        if(logo==0)logo=getResources().getIdentifier("raitchi_logo","drawable",getPackageName());
        if(logo!=0){ImageView icon=new ImageView(this);icon.setImageResource(logo);icon.setScaleType(ImageView.ScaleType.FIT_CENTER);icon.setContentDescription("Raitchi");toolbar.addView(icon,new LinearLayout.LayoutParams(dp(42),dp(42)));}
        TextView brand=text("Raitchi",26);brand.setTypeface(Typeface.create("sans-serif-medium",Typeface.NORMAL));brand.setPadding(dp(10),0,0,0);toolbar.addView(brand,new LinearLayout.LayoutParams(0,-2,1));
        Button settings=button("Settings",false,v->startActivity(new Intent(this,SettingsActivity.class)));toolbar.addView(settings);
        ScrollView scroll=new ScrollView(this);scroll.setFillViewport(true);scroll.setClipToPadding(false);root.addView(scroll,new LinearLayout.LayoutParams(-1,0,1));page=column();page.setPadding(0,dp(shortLandscape()?8:14),0,dp(8));scroll.addView(page);
        if(detailTitle==null)library();else details(detailTitle);
        message=text(operationMessage,13);message.setTextColor(colors.muted);message.setAccessibilityLiveRegion(View.ACCESSIBILITY_LIVE_REGION_POLITE);root.addView(message,wrap());update();
    }
    private void library(){
        LinearLayout heading=row();page.addView(heading,wrap());LinearLayout labels=column();heading.addView(labels,new LinearLayout.LayoutParams(0,-2,1));labels.addView(text("Your library",shortLandscape()?22:26));summary=text("",13);summary.setTextColor(colors.muted);labels.addView(summary);
        Button layout=button(listMode?"Grid view":"List view",false,v->{listMode=!listMode;getSharedPreferences("raitchi_library",MODE_PRIVATE).edit().putBoolean("list",listMode).apply();render();});heading.addView(layout);
        Button add=button("＋  Add game",true,v->chooseTitle());LinearLayout.LayoutParams addParams=new LinearLayout.LayoutParams(-2,-2);addParams.leftMargin=dp(10);heading.addView(add,addParams);gap(page,10);
        if(listMode){for(String t:SharedFiles.TITLES){page.addView(gameCard(t,true),wrap());gap(page,8);}}
        else {LinearLayout cards=row();cards.setGravity(Gravity.TOP);page.addView(cards,wrap());for(int i=0;i<SharedFiles.TITLES.length;i++){LinearLayout.LayoutParams cp=new LinearLayout.LayoutParams(0,-2,1);if(i>0)cp.leftMargin=dp(12);cards.addView(gameCard(SharedFiles.TITLES[i],false),cp);}}
        gap(page,12);TextView hint=text("Add your game files, prepare once, then play. Your saves stay with each game.",13);hint.setTextColor(colors.muted);page.addView(hint);
    }
    private View gameCard(String title,boolean compact){
        LinearLayout card=compact?row():column();card.setBackground(RaitchiUi.rounded(colors.surface,dp(18)));card.setPadding(dp(12),dp(10),dp(12),dp(10));card.setFocusable(true);card.setClickable(true);card.setContentDescription(SharedFiles.label(title)+", game details");card.setOnClickListener(v->{detailTitle=title;render();});
        int art=compact?(shortLandscape()?72:88):libraryArtHeight();
        View cover=cover(title,art);if(compact)card.addView(cover,new LinearLayout.LayoutParams(dp(shortLandscape()?112:132),dp(art)));else card.addView(cover,new LinearLayout.LayoutParams(-1,dp(art)));
        LinearLayout labels=column();if(compact){LinearLayout.LayoutParams lp=new LinearLayout.LayoutParams(0,-2,1);lp.leftMargin=dp(16);card.addView(labels,lp);}else{gap(card,8);card.addView(labels,wrap());}
        TextView name=text(SharedFiles.label(title),shortLandscape()?19:21);name.setTypeface(Typeface.create("sans-serif-medium",Typeface.NORMAL));labels.addView(name);gap(labels,4);TextView status=text("",14);status.setTextColor(colors.accent);labels.addView(status);statuses.put(title,status);
        if(compact){TextView arrow=text("›",30);arrow.setPadding(dp(16),0,dp(8),0);card.addView(arrow);}else{gap(labels,8);TextView open=text("Game details  ›",13);open.setTextColor(colors.muted);labels.addView(open);}return card;
    }
    private View cover(String title,int height){
        FrameLayout box=new FrameLayout(this);int[] shades="las".equals(title)?new int[]{Color.parseColor(colors.dark?"#623247":"#F4BACD"),Color.parseColor(colors.dark?"#382234":"#F9D7D2")}:new int[]{Color.parseColor(colors.dark?"#6A3C36":"#EBC4AF"),Color.parseColor(colors.dark?"#412738":"#F3D6DE")};GradientDrawable backdrop=new GradientDrawable(GradientDrawable.Orientation.TL_BR,shades);backdrop.setCornerRadius(dp(14));box.setBackground(backdrop);
        TextView initials=text("las".equals(title)?"LA":"wonder".equals(title)?"W":"echoes".equals(title)?"EoW":"E",height>120?66:42);initials.setTextColor(Color.parseColor(colors.dark?"#FFE4ED":"#71304B"));initials.setTypeface(Typeface.create("serif",Typeface.BOLD));initials.setGravity(Gravity.CENTER);box.addView(initials,new FrameLayout.LayoutParams(-1,-1));
        if(height>=88){TextView tag=text("NINTENDO SWITCH",10);tag.setLetterSpacing(.16f);tag.setTextColor(Color.parseColor(colors.dark?"#F5D6E2":"#71304B"));FrameLayout.LayoutParams tp=new FrameLayout.LayoutParams(-2,-2,Gravity.BOTTOM|Gravity.START);tp.setMargins(dp(14),0,0,dp(10));box.addView(tag,tp);}return box;
    }
    private void details(String title){
        summary=null;Button back=button("‹  Library",false,v->{detailTitle=null;render();});page.addView(back,new LinearLayout.LayoutParams(-2,-2));gap(page,18);
        LinearLayout body=row();body.setGravity(Gravity.TOP);page.addView(body,wrap());body.addView(cover(title,260),new LinearLayout.LayoutParams(dp(230),dp(260)));
        LinearLayout info=column();LinearLayout.LayoutParams ip=new LinearLayout.LayoutParams(0,-2,1);ip.leftMargin=dp(28);body.addView(info,ip);
        TextView titleView=text(SharedFiles.label(title),28);titleView.setTypeface(Typeface.create("sans-serif-medium",Typeface.NORMAL));info.addView(titleView);gap(info,8);
        detailStatus=text("",15);detailStatus.setTextColor(colors.accent);info.addView(detailStatus);statuses.put(title,detailStatus);gap(info,16);
        LinearLayout actions=row();info.addView(actions,wrap());launchButton=button("Play",true,v->play(title));actions.addView(launchButton);
        compileButton=button("Prepare game",false,v->compile(title));LinearLayout.LayoutParams bp=new LinearLayout.LayoutParams(-2,-2);bp.leftMargin=dp(8);actions.addView(compileButton,bp);
        gap(info,8);LinearLayout more=row();info.addView(more,wrap());importButton=button("Import game files",false,v->chooseImport(title));more.addView(importButton);
        Button perGame=button("Game settings",false,v->startActivity(new Intent(this,SettingsActivity.class).putExtra(SettingsActivity.EXTRA_TITLE,title)));LinearLayout.LayoutParams gp=new LinearLayout.LayoutParams(-2,-2);gp.leftMargin=dp(8);more.addView(perGame,gp);
        gap(info,12);detailProgress=text("",13);detailProgress.setTextColor(colors.muted);info.addView(detailProgress);progressBar=new ProgressBar(this,null,android.R.attr.progressBarStyleHorizontal);progressBar.setProgressTintList(android.content.res.ColorStateList.valueOf(colors.accent));info.addView(progressBar,wrap());
        LinearLayout sessionActions=row();info.addView(sessionActions,wrap());cancelButton=button("Pause preparation",false,v->cancelBuild());sessionActions.addView(cancelButton);endButton=button("End game",false,v->endGame());sessionActions.addView(endButton);
        gap(page,18);TextView help=text("Supported base version: 1.0.0. Preparation resumes completed work after an interruption. Importing files never replaces an existing save.",13);help.setTextColor(colors.muted);page.addView(help);
    }
    static int gamePid(Context c){return processPid(c,":game");}
    static int processPid(Context c,String suffix){ActivityManager am=c.getSystemService(ActivityManager.class);List<ActivityManager.RunningAppProcessInfo> rows=am.getRunningAppProcesses();if(rows!=null)for(ActivityManager.RunningAppProcessInfo row:rows)if(row.uid==android.os.Process.myUid()&&row.processName.equals(c.getPackageName()+suffix))return row.pid;return 0;}
    private boolean buildRunning(){return SharedFiles.read(new File(getFilesDir(),"builds/active.json")).optBoolean("running")&&processPid(this,":build")!=0;}
    private String stateLabel(String title){return !SharedFiles.imported(this,title)?"Game files needed":SharedFiles.ready(this,title)?"Ready to play":"Ready to prepare";}
    private void update(){
        if(message==null)return;JSONObject active=SharedFiles.read(new File(getFilesDir(),"builds/active.json"));boolean running=active.optBoolean("running")&&processPid(this,":build")!=0;int game=gamePid(this),ready=0;
        if(endingPid!=0&&game!=endingPid){endingPid=0;endingRequestedAt=0;note("Game ended.");}
        if(nativeRevision!=NativePackage.revision()){nativeRevision=NativePackage.revision();if(!NativePackage.status().isEmpty())note(NativePackage.status());}
        message.setText(operationMessage);message.setVisibility(operationMessage.isEmpty()?View.GONE:View.VISIBLE);
        for(String t:SharedFiles.TITLES){if(SharedFiles.ready(this,t))ready++;TextView status=statuses.get(t);if(status!=null){String s=stateLabel(t);if(running&&t.equals(active.optString("title")))s="Preparing game…";status.setText(s);}}
        if(summary!=null)summary.setText(SharedFiles.TITLES.length+" supported games · "+ready+" ready to play");
        if(detailTitle!=null&&launchButton!=null){String t=detailTitle;JSONObject p=SharedFiles.read(SharedFiles.progress(this,t));String progress=SharedFiles.buildStatus(p,active,t);detailProgress.setText(busy()?operationMessage:progress);boolean selectedRunning=game!=0&&t.equals(SharedFiles.read(new File(getFilesDir(),"game-session.json")).optString("title"));
            launchButton.setText(selectedRunning?"Resume game":"Play");launchButton.setEnabled(!busy()&&!running&&SharedFiles.ready(this,t)&&(game==0||selectedRunning));
            boolean nativeOnly=TitlePolicy.nativeOnly(t)||("continuous-nce".equals(new SettingsStore(this).getString(t,"cpu_backend"))&&NativePackage.selection(this,t)!=null);compileButton.setVisibility(nativeOnly?View.GONE:View.VISIBLE);
            String phase=p.optString("state",p.optString("phase",""));compileButton.setText("cancelled".equals(phase)||"failed".equals(phase)?"Resume preparation":SharedFiles.ready(this,t)?"Prepare again":"Prepare game");compileButton.setEnabled(!busy()&&!running&&game==0&&SharedFiles.extractedImported(this,t));importButton.setEnabled(!busy()&&!running&&game==0);
            boolean ownBuild=running&&t.equals(active.optString("title"));progressBar.setVisibility(ownBuild?View.VISIBLE:View.GONE);long total=p.optLong("total",0),done=p.optLong("completed",0);progressBar.setIndeterminate(total<=0);if(total>0){progressBar.setMax(1000);progressBar.setProgress((int)Math.max(0,Math.min(1000,done*1000/total)));}cancelButton.setVisibility(ownBuild?View.VISIBLE:View.GONE);endButton.setVisibility(selectedRunning?View.VISIBLE:View.GONE);
            boolean ending=endingPid!=0&&endingPid==game,forceAvailable=ending&&SystemClock.elapsedRealtime()-endingRequestedAt>=10000;
            endButton.setText(forceAvailable?"Force close":ending?"Ending game…":"End game");endButton.setEnabled(!ending||forceAvailable);
        }
    }
    private void compile(String t){if(busy()||buildRunning()||gamePid(this)!=0){note("Finish the active task before preparing another game.");return;}try{startForegroundService(new Intent(this,SharedBuildService.class).putExtra("title",t));note("Preparing "+SharedFiles.label(t)+". You can return to the library.");}catch(Exception e){note("Cannot prepare game: "+e.getMessage());}}
    private void cancelBuild(){try{String t=SharedFiles.read(new File(getFilesDir(),"builds/active.json")).optString("title");SharedFiles.atomic(SharedFiles.cancel(this,t),"cancel\n");note("Pausing preparation. Completed work is kept for Resume.");}catch(Exception e){note("No active preparation.");}}
    private void endGame(){
        final int pid=gamePid(this);if(pid==0)return;
        if(endingPid==pid){
            if(SystemClock.elapsedRealtime()-endingRequestedAt<10000)return;
            new AlertDialog.Builder(dialogContext()).setTitle("Force close the game?")
                .setMessage("The game has not finished ending. Force closing may lose unsaved progress.")
                .setNegativeButton("Keep waiting",null).setPositiveButton("Force close",(d,w)->{
                    // Revalidate the app's current game process; never act on a stale PID.
                    if(endingPid==pid&&gamePid(this)==pid){android.os.Process.killProcess(pid);note("Force closing game…");}
                    update();
                }).show();return;
        }
        new AlertDialog.Builder(dialogContext()).setTitle("End the running game?").setMessage("Save in the game before ending this session.")
            .setNegativeButton("Keep playing",null).setPositiveButton("End game",(d,w)->{
                if(gamePid(this)!=pid){update();return;}
                try{SharedFiles.atomic(new File(getFilesDir(),"game-stop.request"),pid+"\n");endingPid=pid;endingRequestedAt=SystemClock.elapsedRealtime();note("Ending game…");update();
                    // Android can suspend SDL's window loop while the library is
                    // foreground. Resume it so it can drain and join the game.
                    startActivity(new Intent(this,LasActivity.class).addFlags(Intent.FLAG_ACTIVITY_REORDER_TO_FRONT));}
                catch(IOException error){note("Cannot request game shutdown: "+error.getMessage());}
            }).show();
    }
    private void play(String t){
        if(busy()||buildRunning()){note("Wait for the current import or preparation to finish.");return;}
        try{int pid=gamePid(this);if(pid!=0){String selected=SharedFiles.read(new File(getFilesDir(),"game-session.json")).optString("title");if(!t.equals(selected)){note("End the running game before switching games.");return;}startActivity(new Intent(this,LasActivity.class).addFlags(Intent.FLAG_ACTIVITY_REORDER_TO_FRONT));return;}
            SharedFiles.session(this,t);SharedFiles.atomic(new File(getFilesDir(),"game-session.json"),new JSONObject().put("title",t).toString());startActivity(new Intent(this,LasActivity.class));
        }catch(Exception e){note(e.getMessage());}
    }
    private void chooseTitle(){if(busy()||buildRunning()||gamePid(this)!=0){note("Finish the active task before importing game files.");return;}String[] names=new String[SharedFiles.TITLES.length];for(int i=0;i<names.length;i++)names[i]=SharedFiles.label(SharedFiles.TITLES[i]);new AlertDialog.Builder(dialogContext()).setTitle("Add a game").setItems(names,(d,which)->chooseImport(SharedFiles.TITLES[which])).setNegativeButton("Cancel",null).show();}
    private void chooseImport(String t){
        if(busy()||buildRunning()||gamePid(this)!=0){note("Finish the active task before importing game files.");return;}
        importing=t;AlertDialog.Builder dialog=new AlertDialog.Builder(dialogContext()).setTitle("Import "+SharedFiles.label(t))
            .setMessage("Choose a base NSP/XCI and your console keys. Raitchi prepares the original program and reads game data from that file. Keep the selected file available. NSZ/XCZ is not supported.")
            .setPositiveButton("Game file",(d,w)->pickFile()).setNegativeButton("Cancel",null);
        if(!TitlePolicy.nativeOnly(t))dialog.setNeutralButton("Extracted folder",(d,w)->pickFolder());dialog.show();
    }
    private void pickFolder(){startActivityForResult(new Intent(Intent.ACTION_OPEN_DOCUMENT_TREE).addFlags(Intent.FLAG_GRANT_READ_URI_PERMISSION|Intent.FLAG_GRANT_PERSISTABLE_URI_PERMISSION),71);}
    private void pickFile(){startActivityForResult(new Intent(Intent.ACTION_OPEN_DOCUMENT).setType("*/*").addCategory(Intent.CATEGORY_OPENABLE).addFlags(Intent.FLAG_GRANT_READ_URI_PERMISSION|Intent.FLAG_GRANT_PERSISTABLE_URI_PERMISSION),74);}
    private void keepPermission(Uri uri){try{getContentResolver().takePersistableUriPermission(uri,Intent.FLAG_GRANT_READ_URI_PERMISSION);}catch(SecurityException ignored){}}
    private String displayName(Uri uri){try(Cursor c=getContentResolver().query(uri,new String[]{OpenableColumns.DISPLAY_NAME},null,null,null)){if(c!=null&&c.moveToFirst())return c.getString(0);}catch(Exception ignored){}return "game file";}
    @Override protected void onActivityResult(int request,int result,Intent data){
        super.onActivityResult(request,result,data);if(result!=RESULT_OK||data==null||data.getData()==null){if(request==75){packageUri=null;note("Import cancelled. No game files changed.");}return;}Uri uri=data.getData();if(importing==null)return;
        if(busy()||buildRunning()||gamePid(this)!=0){note("Finish the active task before importing game files.");return;}
        if(request==74){String name=displayName(uri).toLowerCase(Locale.ROOT);if(name.endsWith(".nsz")||name.endsWith(".xcz")){note("NSZ/XCZ is not supported. Select an NSP/XCI or an extracted folder.");return;}if(!name.endsWith(".nsp")&&!name.endsWith(".xci")){note("Select an uncompressed NSP or XCI game file.");return;}packageUri=uri;keepPermission(uri);note("Choose your prod.keys file to read this game package.");startActivityForResult(new Intent(Intent.ACTION_OPEN_DOCUMENT).setType("*/*").addCategory(Intent.CATEGORY_OPENABLE).addFlags(Intent.FLAG_GRANT_READ_URI_PERMISSION),75);return;}
        String t=SharedFiles.checked(importing);if(request==75&&packageUri!=null){Uri input=packageUri;packageUri=null;importPackage(t,input,uri);return;}if(request!=71)return;keepPermission(uri);copying=true;note("Importing "+SharedFiles.label(t)+". Keep Raitchi open.");new Thread(()->{try{File target=SharedFiles.game(this,t);copyTree(uri,DocumentsContract.getTreeDocumentId(uri),target,new long[]{0},true);if(!SharedFiles.imported(this,t))throw new IOException("Select the folder containing exefs and romfs (or romfs.bin)");runOnUiThread(()->note("Game files imported. Prepare game will verify the supported version."));}catch(Exception e){runOnUiThread(()->note("Import stopped: "+e.getMessage()));}finally{copying=false;}},"raitchi-folder-import").start();
    }
    private void importPackage(String title,Uri input,Uri keysUri){
        try{NativePackage.start(this,title,input,keysUri);note(NativePackage.status());update();}
        catch(IOException error){note(error.getMessage());}
    }
    private void copyTree(Uri tree,String id,File destination,long[] copied,boolean root) throws Exception {
        if(!destination.isDirectory() && !destination.mkdirs()) throw new IOException("Cannot create game folder");
        Uri children=DocumentsContract.buildChildDocumentsUriUsingTree(tree,id);
        String[] columns={DocumentsContract.Document.COLUMN_DOCUMENT_ID,DocumentsContract.Document.COLUMN_DISPLAY_NAME,DocumentsContract.Document.COLUMN_MIME_TYPE,DocumentsContract.Document.COLUMN_SIZE};
        try(Cursor cursor=getContentResolver().query(children,columns,null,null,null)) {
            if(cursor==null) throw new IOException("Folder is unavailable");
            while(cursor.moveToNext()) {
                String child=cursor.getString(0),name=cursor.getString(1),mime=cursor.getString(2);
                if(name==null || name.equals(".") || name.equals("..") || name.contains("/") || name.contains("\\")) throw new IOException("Invalid game file name");
                // Only imported executable/assets trees belong here; never copy save/key/build folders.
                if(root && !(name.equals("exefs")||name.equals("romfs")||name.equals("romfs.bin"))) continue;
                File file=new File(destination,name);
                if(DocumentsContract.Document.MIME_TYPE_DIR.equals(mime)) { copyTree(tree,child,file,copied,false); continue; }
                Uri document=DocumentsContract.buildDocumentUriUsingTree(tree,child);
                long size=cursor.isNull(3)?-1:cursor.getLong(3);
                if(file.isFile()) {
                    if(size>=0 && file.length()!=size) throw new IOException("Existing file differs: "+name);
                    try(InputStream in=getContentResolver().openInputStream(document);InputStream old=new FileInputStream(file)) {
                        if(in==null || !Arrays.equals(digest(in),digest(old))) throw new IOException("Existing file differs: "+name);
                    }
                    continue;
                }
                if(size>0 && destination.getUsableSpace()<size+(512L<<20)) throw new IOException("Not enough free storage");
                File part=new File(destination,"."+name+".incoming");
                try(InputStream in=getContentResolver().openInputStream(document);FileOutputStream out=new FileOutputStream(part)) {
                    if(in==null) throw new IOException("File is unavailable"); byte[] buffer=new byte[1<<20]; int n;
                    while((n=in.read(buffer))!=-1) { out.write(buffer,0,n); copied[0]+=n; }
                    out.getFD().sync();
                }
                if(size>=0 && part.length()!=size) throw new IOException("Incomplete file: "+name);
                if(!part.renameTo(file)) throw new IOException("Cannot finish file import");
                long amount=copied[0]>>20; runOnUiThread(()->note("Importing… "+amount+" MB copied"));
            }
        }
    }
    private static byte[] digest(InputStream in) throws Exception { MessageDigest sha=MessageDigest.getInstance("SHA-256");byte[] b=new byte[1<<20];int n;while((n=in.read(b))!=-1)sha.update(b,0,n);return sha.digest(); }
}
