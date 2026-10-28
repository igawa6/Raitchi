"""Host-only staging policy/inventory guards; fake files/ADB, no device writes."""
import contextlib,importlib.util,io,json,shutil,subprocess,sys,tempfile,unittest
from pathlib import Path
from unittest.mock import patch
p=Path(__file__).resolve().parents[1]/'tools/stage_shared_manual.py';s=importlib.util.spec_from_file_location('stage',p);m=importlib.util.module_from_spec(s);s.loader.exec_module(m)
class StageSafety(unittest.TestCase):
 def test_environment(self):
  for t in ('las','eevee'):
   text=m.baseline_environment(t);values=dict(x.split('=',1) for x in text.splitlines())
   self.assertEqual(values['LARECOMP_TITLE'],t);self.assertIn('/games/'+t+'/',values['LARECOMP_GAME_ROOT']);self.assertIn('/sessions/'+t+'-1.0.0/',values['LARECOMP_SETTINGS']);self.assertEqual(values['LARECOMP_NPAD'],'fullkey' if t=='las' else 'handheld');self.assertFalse(any('INPUT' in k or 'EXIT' in k for k in values))
   self.assertEqual(values['LARECOMP_VK_DYN_VERTEX'],'0')
   self.assertEqual('LARECOMP_VK_ACQUIRE_FENCE' in values,t=='eevee')
  self.assertIn('LARECOMP_DEBUG_FRAME=120',m.baseline_environment('las',True))
 def test_unknown_title(self):
  with self.assertRaises(ValueError):m.baseline_environment('unknown')
 def test_manual_guard_before_adb(self):
  with tempfile.TemporaryDirectory() as d:
   root=Path(d);(root/'evidence').mkdir();(root/'evidence/manual-session.json').write_text('{"active":true}')
   for title in ('las','eevee'):
    with patch.object(m,'ROOT',root),patch.object(sys,'argv',['stage','--serial','host-test','--title',title]),patch.object(m.subprocess,'run') as run,contextlib.redirect_stderr(io.StringIO()):
     with self.assertRaises(SystemExit) as caught:m.main()
     self.assertEqual(caught.exception.code,2);run.assert_not_called()
 def test_las_inventory_cache_and_links(self):
  with tempfile.TemporaryDirectory() as d:
   root=Path(d);original=root/'private/las-baseline/extracted';(original/'romfs').mkdir(parents=True);(original/'exefs').mkdir();(original/'romfs/asset with space').write_bytes(b'assets');(original/'exefs/main.npdm').write_bytes(b'metadata');(root/'evidence').mkdir();(root/'evidence/las-inputs.json').write_text(json.dumps({'destination':str(original),'romfs_file_count':1}))
   with patch.object(m,'ROOT',root),contextlib.redirect_stdout(io.StringIO()):
    hashes,record=m.las_asset_manifest();self.assertEqual(record['romfs_files'],1);self.assertEqual(record['romfs_bytes'],6);self.assertEqual(set(hashes),{'romfs/asset with space','exefs/main.npdm'});self.assertEqual(m.las_asset_manifest()[0],hashes)
    (original/'romfs/asset with space').write_bytes(b'changed');self.assertNotEqual(m.las_asset_manifest()[0],hashes)
    (original/'romfs/link').symlink_to('asset with space')
    with self.assertRaises(RuntimeError):m.las_asset_manifest()
 def test_process_guard_before_device_writes(self):
  with tempfile.TemporaryDirectory() as d:
   root=Path(d);(root/'build/shared-android').mkdir(parents=True);apk=root/'build/shared-android/test.apk';apk.write_bytes(b'apk')
   (root/'build/shared-android/apk.json').write_text(json.dumps({'runtime_sha256':'runtime','apk':str(apk.relative_to(root)),'apk_sha256':m.sha(apk)}));(root/'build/shared-android/artifact-verification.json').write_text(json.dumps({'two_title_host_gate':True,'runtime_sha256':'runtime'}))
   for title in ('las','eevee'):
    p=root/'build/shared-gamelib'/title;p.mkdir(parents=True);lib=p/'liblasgame.so';lib.write_bytes(b'library');side=p/'liblasgame.so.saotm';side.write_bytes(b'manifest');(p/'build.json').write_text(json.dumps({'semantic':{'title':title},'runtime_sha256':'runtime','library_sha256':m.sha(lib),'sidecar_sha256':m.sha(side)}))
    calls=[]
    def fake(cmd,**kwargs):
     calls.append(cmd);return subprocess.CompletedProcess(cmd,0,b'AYN Thor\n' if 'getprop' in cmd[-1] else b'1234\n',b'')
    with patch.object(m,'ROOT',root),patch.object(sys,'argv',['stage','--serial','host-test','--title',title]),patch.object(m.subprocess,'run',side_effect=fake):
     with self.assertRaisesRegex(RuntimeError,'app is running'):m.main()
    self.assertEqual(len(calls),2);self.assertIn('pidof',calls[-1][-1]);self.assertFalse(any('mkdir' in c[-1] for c in calls))
 def driver_callbacks(self,root):
  def shell(parts,check=True,**kwargs):
   self.assertEqual(parts[:2],['run-as',m.PACKAGE])
   return subprocess.run(parts[2:],cwd=root,check=check,capture_output=True,**kwargs)
  def app_sha(name):
   path=root/name
   return m.sha(path) if path.is_file() else None
  return shell,app_sha
 def test_preserves_existing_driver_and_probe(self):
  with tempfile.TemporaryDirectory() as d:
   root=Path(d);driver=root/'files/gpu_driver/current';driver.mkdir(parents=True)
   (driver/'meta.json').write_text(json.dumps({'libraryName':'libvulkan_custom.so','name':'owner driver'}));(driver/'libvulkan_custom.so').write_bytes(b'new driver');(driver/'.probe').write_bytes(b'preserve probe')
   before={p.name:p.read_bytes() for p in driver.iterdir()};shell,app_sha=self.driver_callbacks(root)
   with patch.object(m,'ROOT',root):
    copy=unittest.mock.Mock();result=m.stage_las_driver(shell,app_sha,copy,'reference')
    copy.assert_not_called();self.assertEqual(result['selection'],'selected-existing');self.assertEqual(result['library_sha256'],app_sha('files/gpu_driver/current/libvulkan_custom.so'));self.assertEqual(result['metadata_sha256'],app_sha('files/gpu_driver/current/meta.json'));self.assertEqual(before,{p.name:p.read_bytes() for p in driver.iterdir()})
 def test_rejects_unsafe_library_names(self):
  for name in ('../lib.so','/lib.so','a/b.so','a\\b.so','..','.', '', 'lib\n.so','lib\r.so','lib\x00.so','meta.json',None,42):
   with self.subTest(name=name):
    with self.assertRaisesRegex(RuntimeError,'basename'):m.driver_library_name({'libraryName':name})
 def test_rejects_nonempty_invalid_driver_without_copy(self):
  for scenario in ('missing-meta','invalid-json','missing-library','unsafe-name','linked-library','linked-meta','linked-directory','linked-parent','changed-meta'):
   with self.subTest(scenario=scenario),tempfile.TemporaryDirectory() as d:
    root=Path(d);driver=root/'files/gpu_driver/current';driver.mkdir(parents=True);(driver/'leftover').write_bytes(b'keep')
    if scenario!='missing-meta':(driver/'meta.json').write_text('{' if scenario=='invalid-json' else '{"libraryName":"lib.so"}')
    if scenario=='unsafe-name':(driver/'meta.json').write_text('{"libraryName":"../outside.so"}')
    if scenario in ('linked-meta','linked-library','changed-meta'):(driver/'lib.so').write_bytes(b'keep-library')
    if scenario=='linked-library':(driver/'lib.so').unlink();(driver/'lib.so').symlink_to('leftover')
    if scenario=='linked-meta':(driver/'meta.json').rename(driver/'real-meta');(driver/'meta.json').symlink_to('real-meta')
    if scenario=='linked-directory':driver.rename(driver.parent/'original');driver.symlink_to('original',target_is_directory=True)
    if scenario=='linked-parent':driver.parent.rename(root/'files/original');(root/'files/gpu_driver').symlink_to('original',target_is_directory=True)
    shell,app_sha=self.driver_callbacks(root)
    if scenario=='changed-meta':
     original_sha=app_sha
     app_sha=lambda p: '0'*64 if p.endswith('/meta.json') else original_sha(p)
    copy=unittest.mock.Mock()
    with self.assertRaises(RuntimeError):m.stage_las_driver(shell,app_sha,copy,'reference')
    copy.assert_not_called();self.assertEqual((driver/'leftover').read_bytes(),b'keep')
 def test_empty_directory_seeds_verified_reference(self):
  with tempfile.TemporaryDirectory() as d:
   root=Path(d);bundle=root/'work/las-baseline/build/android/deps/turnip';bundle.mkdir(parents=True);(bundle/'meta.json').write_text('{"libraryName":"lib.so"}');(bundle/'lib.so').write_bytes(b'pinned');(root/'files').mkdir();digest=m.sha(bundle/'lib.so');local_shell,app_sha=self.driver_callbacks(root);copies=[]
   def shell(parts,**kwargs):
    if parts[:2]==['run-as','reference']:
     name=Path(parts[-1]).name;return subprocess.CompletedProcess(parts,0,(m.sha(bundle/name)+'  '+name+'\n').encode(),b'')
    return local_shell(parts,**kwargs)
   def copy(names,source,destination,log):
    copies.append(names)
    for name in names:shutil.copy2(bundle/name,root/destination/name)
   with patch.object(m,'ROOT',root),patch.object(m,'PINNED_TURNIP',digest):
    result=m.stage_las_driver(shell,app_sha,copy,'reference')
   self.assertEqual(result['selection'],'pinned-reference-turnip');self.assertEqual(result['library_sha256'],digest);self.assertEqual(copies,[['meta.json','lib.so']])
unittest.main(verbosity=2)
