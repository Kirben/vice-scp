from pathlib import Path
import hashlib,json,os,shutil,socket,struct,subprocess,time

import argparse
parser=argparse.ArgumentParser(description='Generated SCP native-drive and snapshot acceptance checks. Requires a native Windows runtime with ROMs/DLLs; uses no user images.')
parser.add_argument('output',type=Path)
parser.add_argument('--runtime',required=True,type=Path)
parser.add_argument('--reference-runtime',type=Path)
parser.add_argument('--legacy-snapshot',type=Path)
parser.add_argument('--disk-tool',type=Path,help='c1541 fixture generator (defaults to RUNTIME/c1541.exe)')
parser.add_argument('--multiunit-only',action='store_true',help='Run generated two-unit native capture/snapshot tests')
parser.add_argument('--multirev-only',action='store_true',help='Run generated multi-revolution and disk-replacement checks')
parser.add_argument('--capture-speed-only',action='store_true',help='Run generated 360 RPM playback and speed snapshot checks')
parser.add_argument('--blank-turn-only',action='store_true',help='Diagnose native blank-turn loading with DOS status and independent timing oracles')
parser.add_argument('--blank-turn-case',action='append',help='Select a named blank-turn diagnostic case (repeatable)')
parser.add_argument('--blank-turn-trace',action='store_true',help='Trace verified 1541-II ROM sync success/timeout checkpoints')
parser.add_argument('--pulse-fixture-tool',type=Path,help='Exact 200 ms SCP-to-P64 diagnostic exporter (requires --reference-runtime)')
args=parser.parse_args()
app=args.runtime.resolve();out=args.output.resolve()
out.mkdir(parents=True,exist_ok=False)
env=os.environ.copy()
env.update(SDL_VIDEODRIVER='dummy',SDL_AUDIODRIVER='dummy',SDL_RENDER_DRIVER='software')
env['PATH']=str(app)+os.pathsep+env.get('SystemRoot','C:/Windows')+'/System32'
for key in ['TEMP','TMP','APPDATA','LOCALAPPDATA']:
 directory=out/key;directory.mkdir();env[key]=str(directory)
success=out/'success.prg'
success.write_bytes(bytes.fromhex('01080b080a009e32303631000000a9008dffd74c1208'))
g64_path=out/'generated.g64'
created=subprocess.run([str(args.disk_tool.resolve() if args.disk_tool else app/'c1541.exe'),'-format','native,01','g64',str(g64_path),
                        '-write',str(success),'success'],capture_output=True,env=env,timeout=30)
assert created.returncode==0 and g64_path.exists(),created.stderr
g64_path.chmod(0o444)
g64=g64_path.read_bytes()
n=g64[9];data=bytearray(688);tracks=[]
for ht in range(n):
 offset,=struct.unpack_from('<I',g64,12+ht*4)
 if not offset:continue
 size,=struct.unpack_from('<H',g64,offset)
 raw=g64[offset+2:offset+2+size]
 zone,=struct.unpack_from('<I',g64,12+n*4+ht*4)
 assert zone<=3
 bit_ticks=(16-zone)*4*5
 intervals=[];previous=0
 for bit in range(size*8):
  if not raw[bit//8] & (0x80>>(bit%8)):continue
  at=(bit+1)*bit_ticks
  if at>16000000:continue
  delta=(at-previous)//2
  while delta>65535:intervals.append(0);delta-=65536
  assert delta>0
  intervals.append(delta);previous=at
 struct.pack_into('<I',data,16+ht*4,len(data))
 data+=b'TRK'+bytes([ht])+struct.pack('<III',8000000,len(intervals),16)
 data+=struct.pack('>'+'H'*len(intervals),*intervals)
 tracks.append(ht)
data[:16]=struct.pack('<3s9BI',b'SCP',0x17,0,1,min(tracks),max(tracks),1,0,1,0,0)
struct.pack_into('<I',data,12,sum(data[16:])&0xffffffff)
image=out/'generated.scp'
if image.exists():image.chmod(0o666)
image.write_bytes(data);image.chmod(0o444)
changed=bytearray(data);offset,=struct.unpack_from('<I',changed,16)
word,=struct.unpack_from('>H',changed,offset+16)
struct.pack_into('>H',changed,offset+16,word+1)
struct.pack_into('<I',changed,12,sum(changed[16:])&0xffffffff)
alternate=out/'different.scp'
if alternate.exists():alternate.chmod(0o666)
alternate.write_bytes(changed);alternate.chmod(0o444)

basic=bytes.fromhex('0b080a009e32303631000000')
ram=bytearray(basic+bytes.fromhex('4c0d08'))
ram+=bytes(0xc000-0x0801-len(ram))
ram+=bytes.fromhex('a907a228a0c020bdffa901a208a00120baffa900a200a00020d5ffb0034c0d08a9ff8dffd74c25c0')+b'SUCCESS'
idle=out/'idle.prg';idle.write_bytes(b'\x01\x08'+ram)
results={}

class Session:
 def __init__(self,name,disk=image,pristine=False,program=idle,model=1542,additional_disks=None,remote_monitor=False,flux_speed=None):
  self.directory=out/name;self.directory.mkdir(exist_ok=True)
  self.log=self.directory/'vice.log'
  if self.log.exists():self.log.unlink()
  self.request=0
  self.events=[]
  self.checkpoint_hits=[]
  with socket.socket() as allocator:
   allocator.bind(('127.0.0.1',0));port=allocator.getsockname()[1]
  runtime=args.reference_runtime.resolve() if pristine else app
  argv=[runtime/'x64sc.exe','-default','-console','-warp','-sound','-sounddev','dummy',
    '-sidenginemodel','256','-debugcart','-binarymonitor','-binarymonitoraddress',f'127.0.0.1:{port}',
    '-logfile',self.log,'-attach8ro','-drive8type',str(model),'-drive8truedrive','+trapdevice8',
    '+autostart-handle-tde','-autostart-delay','0','-limitcycles','600000000',
    '-autostartprgmode','1','-autostart',program]
  if flux_speed is not None:argv+=['-drive8fluxspeed',str(flux_speed)]
  if disk is not None:argv+=['-8',disk]
  if remote_monitor:
   with socket.socket() as allocator:
    allocator.bind(('127.0.0.1',0));self.remote_port=allocator.getsockname()[1]
   argv+=['-remotemonitor','-remotemonitoraddress',f'127.0.0.1:{self.remote_port}']
  for unit,extra_disk in (additional_disks or {}).items():
   argv+=['-attach%dro'%unit,'-drive%dtype'%unit,'1542','-drive%dtruedrive'%unit,'+trapdevice%d'%unit]
   if extra_disk is not None:argv+=['-%d'%unit,extra_disk]
  self.process=subprocess.Popen([str(x) for x in argv],cwd=self.directory,env=env,
    stdin=subprocess.DEVNULL,stdout=subprocess.DEVNULL,stderr=subprocess.DEVNULL,creationflags=subprocess.CREATE_NO_WINDOW)
  self.connection=None
  try:
   deadline=time.monotonic()+25
   while time.monotonic()<deadline:
    if self.process.poll() is not None:raise AssertionError((self.process.returncode,self.log.read_text(errors='replace')[-5000:]))
    if self.log.exists() and 'Autostart: Done.' in self.log.read_text(errors='replace'):
     self.connection=socket.create_connection(('127.0.0.1',port),timeout=10);break
    time.sleep(.03)
   assert self.connection is not None,self.log.read_text(errors='replace')[-5000:]
   self.connection.settimeout(20);self.request=0
   self.command(0x81)
  except BaseException:
   self.close();raise
 def command(self,code,body=b''):
  self.request+=1
  self.connection.sendall(b'\x02\x02'+struct.pack('<II',len(body),self.request)+bytes([code])+body)
  def read(n):
   data=b''
   while len(data)<n:
    part=self.connection.recv(n-len(data));assert part;data+=part
   return data
  while True:
   header=read(12);length,=struct.unpack_from('<I',header,2);assert length<10000000
   payload=read(length);rid,=struct.unpack_from('<I',header,8)
   if rid==self.request:return header[7],payload
   self.events.append((header[6],payload))
 def wait_stop(self,address=None):
  def read(n):
   data=b''
   while len(data)<n:
    part=self.connection.recv(n-len(data));assert part;data+=part
   return data
  while True:
   if self.events:
    kind,payload=self.events.pop(0)
   else:
    header=read(12);length,=struct.unpack_from('<I',header,2)
    assert length<10000000
    kind,payload=header[6],read(length)
   if kind==0x11 and len(payload)==23 and payload[4]:self.checkpoint_hits.append(payload)
   if kind==0x62 and (address is None or struct.unpack('<H',payload)[0]==address):return struct.unpack('<H',payload)[0]
 def save(self,name,embed):
  path=self.directory/name;raw=path.as_posix().encode()
  rc,_=self.command(0x41,bytes([0,embed,len(raw)])+raw);assert rc==0,rc
  return path
 def restore(self,path):
  raw=path.as_posix().encode();return self.command(0x42,bytes([len(raw)])+raw)[0]
 def load_program(self,address=0xc000):
  resource=b'DebugCartEnable'
  rc,_=self.command(0x52,bytes([1,len(resource)])+resource+bytes([1,1]));assert rc==0
  if address is not None:
   rc,_=self.command(0x32,bytes([0,1,0,3,3,address&255,address>>8]));assert rc==0
  rc,_=self.command(0xaa);assert rc==0
  code=self.process.wait(timeout=40)
  assert code==0,(code,self.log.read_text(errors='replace')[-5000:])
 def close(self):
  if self.connection:
   try:
    if self.process.poll() is None:self.command(0xbb)
   except (OSError,AssertionError):pass
   self.connection.close()
  if self.process.poll() is None:
   try:self.process.wait(timeout=3)
   except subprocess.TimeoutExpired:self.process.kill();self.process.wait(timeout=10)

if args.capture_speed_only:
 from test_capture_speed import run
 run(Session,out,image,success,env,app)
 raise SystemExit(0)

if args.blank_turn_only:
 from test_blank_turn import run
 run(Session,out,image,success,basic,env,app,args.blank_turn_case,args.blank_turn_trace,args.pulse_fixture_tool)
 raise SystemExit(0)

if args.multirev_only:
 from test_multirev import run
 run(Session,out,image,success,basic,env,app)
 raise SystemExit(0)

if args.multiunit_only:
 from test_multiunit import run
 run(Session,out,image,success,basic,env,app,args.disk_tool)
 raise SystemExit(0)

before=hashlib.sha256(image.read_bytes()).hexdigest()
for model in [1541, 1542]:
 for handle in [True, False]:
  directory=out/('scp-autostart-%d-%d' % (model, handle));directory.mkdir()
  log=directory/'vice.log'
  argv=[app/'x64sc.exe','-default','-console','-warp','+sound','-debugcart',
    '-logfile',log,'-attach8ro','-drive8type',str(model),'-drive8truedrive','+trapdevice8',
    '-autostart-handle-tde' if handle else '+autostart-handle-tde',
    '-autostart-delay','0','-limitcycles','600000000','-autostart',image]
  completed=subprocess.run([str(x) for x in argv],cwd=directory,env=env,
    stdout=subprocess.DEVNULL,stderr=subprocess.DEVNULL,timeout=60,
    creationflags=subprocess.CREATE_NO_WINDOW)
  text=log.read_text(errors='replace')
  assert completed.returncode==0,(completed.returncode,text[-5000:])
  assert 'Autostart: Done.' in text and 'Autostart: Loading program' in text,text[-5000:]
  assert 'cycle limit' not in text.lower(),text[-5000:]
  results['native_scp_autostart_%d_handle_%d' % (model,handle)]=True
assert hashlib.sha256(image.read_bytes()).hexdigest()==before
session=Session('native-numbered-autostart')
try:
 resource=b'AutostartHandleTrueDriveEmulation'
 rc,_=session.command(0x52,bytes([1,len(resource)])+resource+bytes([1,1]));assert rc==0
 raw=image.as_posix().encode()
 rc,_=session.command(0xdd,bytes([1,1,0,len(raw)])+raw);assert rc==0
 rc,_=session.command(0xaa);assert rc==0
 code=session.process.wait(timeout=60)
 text=session.log.read_text(errors='replace')
 assert code==0 and "Loading program 'SUCCESS'" in text,(code,text[-5000:])
 results['native_scp_directory_number_selection_autostart']=True
finally:session.close()
extracted=out/'extracted.prg'
readback=subprocess.run([str(app/'c1541.exe'),'-attach',str(image),'-list',
    '-read','success',str(extracted)],capture_output=True,env=env,timeout=60)
assert readback.returncode==0 and extracted.read_bytes()==success.read_bytes(),readback.stderr
assert b'"success"' in readback.stdout.lower(),readback.stdout
assert hashlib.sha256(image.read_bytes()).hexdigest()==before
results['native_scp_directory_and_exact_extraction']=True
print('Native SCP autostart and independent sector extraction passed.',flush=True)
unsupported=bytearray(data);unsupported[4]=0x40
for name,content,index in [('malformed.prg',b'SCP'+bytes(1021),0),
                           ('short.scp',b'SCP',0),
                           ('unsupported.scp',unsupported,0),
                           ('missing-program.scp',data,65535)]:
 rejected=out/name;rejected.write_bytes(content);rejected.chmod(0o444)
 session=Session('reject-autostart-'+name)
 try:
  def flux_payload(raw):
   start=raw.index(b'FLUX8:0\0');size,=struct.unpack_from('<I',raw,start+18)
   return raw[start+22:start+size]
  state_before=flux_payload(session.save('before.vsf',0).read_bytes())
  raw=rejected.as_posix().encode()
  rc,_=session.command(0xdd,bytes([1,index&255,index>>8,len(raw)])+raw)
  assert rc!=0,'rejected SCP fell through to another autostart type: '+name
  state_after=flux_payload(session.save('after.vsf',0).read_bytes())
  assert state_after==state_before,'failed autostart altered native capture/cursor'
  text=session.log.read_text(errors='replace')
  assert 'SCP autostart failed; not trying other file types.' in text,text[-5000:]
  session.load_program()
  results['rejected_scp_autostart_'+name]=True
 finally:session.close()
 assert rejected.read_bytes()==content
print('Rejected SCP autostart did not fall through; mounted native disk still loads.',flush=True)
session=Session('native-load')
try:session.load_program();results['native_true_drive_load']=True
finally:session.close()
assert hashlib.sha256(image.read_bytes()).hexdigest()==before
session=Session('native-load-1541',model=1541)
try:session.load_program();results['native_1541_true_drive_load']=True
finally:session.close()
print('Native true-drive SCP LOAD passed.',flush=True)
(out/'results.json').write_text(json.dumps(results,indent=2))

# Attempt a real KERNAL SAVE, then read the drive's DOS status channel. A
# protected medium must report 26 (WRITE PROTECT ON), not merely retain bytes.
code=bytearray();branches=[]
def emit(*values):code.extend(values)
def call(address):emit(0x20,address&255,address>>8)
emit(0xa9,7,0xa2,0x80,0xa0,0xc0);call(0xffbd)
emit(0xa9,1,0xa2,8,0xa0,1);call(0xffba)
emit(0xa9,1,0x85,0xfb,0xa9,8,0x85,0xfc)
emit(0xa9,0xfb,0xa2,0x17,0xa0,8);call(0xffd8)
emit(0xa9,0,0xa2,0,0xa0,0);call(0xffbd)
emit(0xa9,15,0xa2,8,0xa0,15);call(0xffba);call(0xffc0)
emit(0xa2,15);call(0xffc6)
for char in b'26':
 call(0xffcf);emit(0xc9,char,0xd0,0);branches.append(len(code)-1)
emit(0xa9,0,0x8d,0xff,0xd7,0x4c,0,0xc0)
failure=len(code);emit(0x8d,0xff,0xd7,0x4c,0,0xc0)
for branch in branches:code[branch]=failure-(branch+1)
assert len(code)<128
code+=bytes(128-len(code))+b'NEWFILE'
save_program=out/'idle-save.prg'
save_program.write_bytes(b'\x01\x08'+basic+bytes.fromhex('4c0d08')+bytes(0xc000-0x0801-len(basic)-3)+code)
session=Session('write-protection',program=save_program)
try:
 resource=b'Drive8TrueEmulation'
 rc,_=session.command(0x52,bytes([1,len(resource)])+resource+bytes([1,0]))
 assert rc!=0,'native image silently switched to virtual drive playback'
 results['disabling_true_drive_rejected']=True
 resource=b'TrapDevice8'
 rc,_=session.command(0x52,bytes([1,len(resource)])+resource+bytes([1,1]))
 assert rc!=0,'native image silently switched to virtual device traps'
 assert 'Native SCP requires disabled device traps for unit 8.' in session.log.read_text(errors='replace')
 results['enabling_device_traps_rejected']=True
 resource=b'TrapDevice9'
 rc,_=session.command(0x52,bytes([1,len(resource)])+resource+bytes([1,1]));assert rc==0
 rc,_=session.command(0x52,bytes([1,len(resource)])+resource+bytes([1,0]));assert rc==0
 results['other_unit_traps_remain_configurable']=True
 resource=b'AttachDevice8d0Readonly'
 rc,_=session.command(0x52,bytes([1,len(resource)])+resource+bytes([1,0]));assert rc==0
 session.load_program()
 results['dos_write_protect_26']=True
 results['dos_write_protected_with_writable_attachment_preference']=True
finally:session.close()
assert hashlib.sha256(image.read_bytes()).hexdigest()==before
print('Native DOS write protection and true-drive guard passed.',flush=True)

def module(data,name):
 start=data.index(name.encode()+b'\0')
 size,=struct.unpack_from('<I',data,start+18)
 assert size>=22 and start+size<=len(data)
 return start,start+22,start+size

session=Session('native-preference-file')
try:
 original=session.save('before.vsf',0).read_bytes();_,p0,e0=module(original,'FLUX8:0')
 resource=b'AttachDevice8d0Readonly'
 for value in [0,1,0,1]:
  rc,_=session.command(0x52,bytes([1,len(resource)])+resource+bytes([1,value]));assert rc==0
  checked=session.save('after-%d.vsf'%value,0).read_bytes();_,p1,e1=module(checked,'FLUX8:0')
  assert original[p0:e0]==checked[p1:e1], 'attachment preference reset native capture/cursor'
 session.load_program()
 results['native_preference_preserves_file_capture_state_and_load']=True
finally:session.close()

session=Session('native-model-settings')
try:
 original=session.save('before.vsf',0).read_bytes()
 resource=b'Drive8Type'
 for model in [1542,1541,1581,1571,0,1542]:
  rc,_=session.command(0x52,bytes([1,len(resource)])+resource+bytes([2,model&255,model>>8]))
  assert (rc==0)==(model==1542), 'model change unexpectedly accepted/rejected'
  checked=session.save('model-%d.vsf'%model,0).read_bytes()
  for name in ['FLUX8:0','DRIVE8']:
   _,p0,e0=module(original,name);_,p1,e1=module(checked,name)
   assert original[p0:e0]==checked[p1:e1], 'model setting altered mounted native state'
 session.load_program()
 results['native_model_settings_preserve_capture_circuit_and_load']=True
finally:session.close()

session=Session('snapshot-save')
try:
 embedded=session.save('embedded.vsf',1)
 external=session.save('external.vsf',0)
 raw=embedded.read_bytes();start,payload,end=module(raw,'FLUX8:0')
 drive_start,_,_=module(raw,'DRIVE8')
 assert raw[drive_start+16:drive_start+18]==b'\x02\x01'
 assert raw[start+16:start+18]==b'\x01\x00'
 assert session.restore(external)==0
 roundtrip=session.save('external-roundtrip.vsf',0).read_bytes()
 _,p2,e2=module(roundtrip,'FLUX8:0')
 raw_external=external.read_bytes();_,pe,ee=module(raw_external,'FLUX8:0')
 assert raw_external[pe:ee]==roundtrip[p2:e2]
 session.load_program()
 results['external_snapshot_matching_capture_and_load']=True
finally:session.close()
print('Native external-capture snapshot round trip passed.',flush=True)

session=Session('mid-transfer-snapshot')
try:
 rc,_=session.command(0x12,struct.pack('<HHBBBBB',0xffd5,0xffd5,1,1,4,1,0));assert rc==0
 rc,_=session.command(0x32,bytes([0,1,0,3,3,0,0xc0]));assert rc==0
 rc,_=session.command(0xaa);assert rc==0
 session.wait_stop(0xffd5)
 for attempt in range(5):
  session.events=[]
  rc,_=session.command(0x71,b'\0'+struct.pack('<H',20000));assert rc==0
  session.wait_stop()
  active=session.save('active.vsf',1)
  active_data=active.read_bytes();_,pa,ea=module(active_data,'FLUX8:0')
  if active_data[pa+54]==1:break
 else:raise AssertionError('Native motor never started during LOAD')
 assert session.restore(active)==0
 checked=session.save('active-roundtrip.vsf',1).read_bytes();_,pc,ec=module(checked,'FLUX8:0')
 assert active_data[pa:ea]==checked[pc:ec]
 resource=b'AttachDevice8d0Readonly'
 for value in [0,1]:
  rc,_=session.command(0x52,bytes([1,len(resource)])+resource+bytes([1,value]));assert rc==0
  toggled=session.save('active-preference-%d.vsf'%value,1).read_bytes();_,pt,et=module(toggled,'FLUX8:0')
  assert active_data[pa:ea]==toggled[pt:et], 'preference changed active native cursor'
 results['native_preference_preserves_active_transfer']=True
 session.load_program(None)
 results['mid_transfer_snapshot_exact_state_and_load']=True
finally:session.close()
print('Active native disk-transfer snapshot round trip passed.',flush=True)

for name,disk in [('wrong-capture',alternate),('missing-capture',None)]:
 session=Session(name,disk=disk)
 try:
  rc=session.restore(external);assert rc!=0,rc
  results[name+'_rejected']=True
 finally:session.close()

offline=out/'generated-offline.scp'
image.rename(offline)
try:
 session=Session('embedded-source-unavailable',disk=alternate)
 try:
  assert session.restore(embedded)==0
  roundtrip=session.save('embedded-roundtrip.vsf',1).read_bytes()
  _,pr,er=module(roundtrip,'FLUX8:0')
  assert raw[payload:end]==roundtrip[pr:er]
  resource=b'AttachDevice8d0Readonly'
  for value in [0,1,0,1]:
   rc,_=session.command(0x52,bytes([1,len(resource)])+resource+bytes([1,value]));assert rc==0
   checked=session.save('preference-%d.vsf'%value,1).read_bytes();_,ps,es=module(checked,'FLUX8:0')
   assert raw[payload:end]==checked[ps:es], 'preference lost or reset embedded capture'
  results['native_preference_preserves_embedded_capture_offline_and_load']=True
  resource=b'Drive8Type'
  for model in [1542,1581]:
   rc,_=session.command(0x52,bytes([1,len(resource)])+resource+bytes([2,model&255,model>>8]))
   assert (rc==0)==(model==1542)
   checked=session.save('model-%d.vsf'%model,1).read_bytes();_,pm,em=module(checked,'FLUX8:0')
   assert raw[payload:end]==checked[pm:em], 'model selection lost embedded native image'
  results['native_model_settings_preserve_embedded_capture_offline']=True
  session.load_program()
  results['embedded_restore_without_source_and_load']=True
 finally:session.close()
finally:offline.rename(image)
print('Embedded snapshot restored and LOAD passed with source unavailable.',flush=True)

if args.reference_runtime:
 session=Session('old-reader',disk=None,pristine=True)
 try:
  rc=session.restore(embedded);assert rc!=0
  assert 'version' in session.log.read_text(errors='replace').lower()
  results['pristine_reader_rejects_required_native_version']=True
 finally:session.close()


variants={}
bad=bytearray(raw);_,dp,_=module(bad,'DRIVE8');struct.pack_into('<I',bad,dp+40,1581);variants['unsupported-drive-model']=bad
bad=bytearray(raw);ds,_,_=module(bad,'DRIVE8');bad[ds+17]=0;variants['lost-backend-version']=bad
bad=bytearray(raw);ds,_,_=module(bad,'DRIVE8');bad[ds:ds+6]=b'LOSTxx';variants['missing-drive-marker']=bad
bad=bytearray(raw);_,dp,_=module(bad,'DRIVE8');struct.pack_into('<I',bad,dp+79,0xffffffff);variants['invalid-read-circuit']=bad
bad=bytearray(raw);bad[start:start+4]=b'LOST';variants['missing-module']=bad
bad=bytearray(raw);bad[start+16]=2;variants['future-module-version']=bad
bad=bytearray(raw);_,_,drive_end=module(bad,'DRIVE8');bad[drive_end-1]=2;variants['unknown-backend']=bad
bad=bytearray(raw);bad[payload+56]=5;variants['invalid-clock-phase']=bad
bad=bytearray(raw);struct.pack_into('<I',bad,payload+70,32000001);variants['oversize-capture']=bad
bad=bytearray(raw);bad[payload+1]^=1;variants['capture-identity']=bad
for name,bad in variants.items():
 path=out/(name+'.vsf');path.write_bytes(bad)
 session=Session(name,disk=alternate)
 try:
  assert session.restore(path)!=0,name
  if name=='unsupported-drive-model':assert 'require a 1541/1541-II' in session.log.read_text(errors='replace')
  results[name+'_rejected']=True
 finally:session.close()
 print(name,'rejected',flush=True)
def rejected_attachment(name,options):
 directory=out/name;directory.mkdir()
 log=directory/'vice.log'
 with socket.socket() as allocator:
  allocator.bind(('127.0.0.1',0));port=allocator.getsockname()[1]
 argv=[app/'x64sc.exe','-default','-console','+sound','-warp','-attach8ro','-8',g64_path,
       '-remotemonitor','-remotemonitoraddress',f'127.0.0.1:{port}','-logfile',log,
       '-limitcycles','600000000']+options
 process=subprocess.Popen([str(x) for x in argv],cwd=directory,env=env,stdout=subprocess.DEVNULL,
    stderr=subprocess.DEVNULL,creationflags=subprocess.CREATE_NO_WINDOW)
 connection=None
 try:
  deadline=time.monotonic()+20
  while time.monotonic()<deadline:
   assert process.poll() is None,process.returncode
   if log.exists() and 'Attach: Disk image unit 8:0' in log.read_text(errors='replace'):
    connection=socket.create_connection(('127.0.0.1',port),timeout=10);break
   time.sleep(.03)
  assert connection is not None
  connection.settimeout(10)
  def command(text,needle):
   connection.sendall(text.encode()+b'\n');response=b''
   while True:
    part=connection.recv(8192);assert part;response+=part
    if needle in response and response.rstrip().endswith(b')'):return response
  first=command('list 8',b'blocks free.')
  command('attach "'+image.as_posix()+'" 8',b'Failed.')
  last=command('list 8',b'blocks free.')
  assert b'"SUCCESS"' in first and b'"SUCCESS"' in last
  assert 'SCP requires a 1541/1541-II' in log.read_text(errors='replace')
  connection.sendall(b'quit\n');assert process.wait(timeout=10)==0
  results[name+'_preserved_existing_image']=True
 finally:
  if connection:connection.close()
  if process.poll() is None:process.kill();process.wait(timeout=10)

rejected_attachment('unsupported-model-attachment',['-drive8type','1571','-drive8truedrive','+trapdevice8'])
rejected_attachment('virtual-drive-attachment',['-drive8type','1542','+drive8truedrive','+trapdevice8'])
rejected_attachment('device-trap-attachment',['-drive8type','1542','-drive8truedrive','-trapdevice8'])
print('Unsupported attachment checks preserved the mounted G64.',flush=True)

if args.legacy_snapshot:
 session=Session('native-to-legacy')
 try:
  assert session.restore(args.legacy_snapshot.resolve())==0
  legacy=session.save('legacy-roundtrip.vsf',1).read_bytes()
  ds,_,_=module(legacy,'DRIVE8')
  assert legacy[ds+16:ds+18]==b'\x02\x00' and b'FLUX8:0' not in legacy
  session.load_program(0x080d)
  results['native_to_legacy_snapshot_and_cleanup']=True
 finally:session.close()
assert hashlib.sha256(image.read_bytes()).hexdigest()==before
results['generated_image_unchanged']=True
results['complete']=True
(out/'results.json').write_text(json.dumps(results,indent=2))
print('NATIVE DRIVE AND SNAPSHOT TESTS COMPLETE',flush=True)
