import contextlib,io,os,sys,json,esptool,ssl,socket,hashlib,re,base64,time
from pathlib import Path
from urllib.request import parse_http_list,parse_keqv_list
port=os.environ.get('ESP_PORT') or (sys.argv[1] if len(sys.argv)>1 else '')
idf=os.environ.get('IDF_PATH','')
if not port or not idf:
    raise SystemExit('Set ESP_PORT (serial device) and IDF_PATH before probing NVS')
print('Reading printer settings from',port,flush=True)
sys.path.insert(0,str(Path(idf)/'components/nvs_flash/nvs_partition_tool'))
from nvs_parser import NVS_Partition
with contextlib.redirect_stdout(io.StringIO()):
    dev=esptool.detect_chip(port);dev=dev.run_stub()
    try:raw=dev.read_flash(0x9000,0x6000)
    finally:dev.hard_reset();dev._port.close()
print('NVS read complete',flush=True)
entries=[e for p in NVS_Partition('settings',bytearray(raw)).pages for e in p.entries if e.state=='Written']
ns=next(e.data['value'] for e in entries if e.metadata['namespace']==0 and e.key=='bambu')
e=next(e for e in entries if e.metadata['namespace']==ns and e.key=='config')
c=json.loads(b''.join(x.raw for x in e.children)[:e.data['size']].rstrip(b'\0'))
print('Connecting H2D',flush=True)
ctx=ssl.SSLContext(ssl.PROTOCOL_TLS_CLIENT);ctx.check_hostname=False;ctx.verify_mode=ssl.CERT_NONE
s=ctx.wrap_socket(socket.create_connection((c['ip0'],322),timeout=15),server_hostname=c['ip0'])
f=s.makefile('rb');url=f"rtsps://{c['ip0']}:322/streaming/live/1";seq=0;challenge=None;session=''
def request(method,target,headers=''):
    global seq,challenge
    seq+=1;auth=''
    if challenge:
        assert 'qop' not in challenge
        md=lambda x:hashlib.md5(x.encode()).hexdigest()
        response=md(md('bblp:'+challenge['realm']+':'+c['code0'])+':'+challenge['nonce']+':'+md(method+':'+target))
        auth=f'Authorization: Digest username="bblp", realm="{challenge["realm"]}", nonce="{challenge["nonce"]}", uri="{target}", response="{response}"\r\n'
    sess=f'Session: {session}\r\n' if session else ''
    s.sendall(f'{method} {target} RTSP/1.0\r\nCSeq: {seq}\r\n{auth}{sess}{headers}\r\n'.encode())
    first=f.read(1)
    while first==b'$':
        prefix=f.read(3);packet=f.read(int.from_bytes(prefix[1:],'big'))
        print('Interleaved channel before reply:',prefix[0],'bytes:',len(packet),flush=True)
        assert prefix[0]==1
        first=f.read(1)
    status=int((first+f.readline()).split()[1]);h={}
    while True:
        line=f.readline()
        if line==b'\r\n':break
        k,v=line.decode().split(':',1);h[k.lower()]=v.strip()
    body=f.read(int(h.get('content-length','0'))).decode()
    if status==401:
        assert challenge is None
        scheme,value=h['www-authenticate'].split(' ',1);assert scheme=='Digest'
        challenge=parse_keqv_list(parse_http_list(value))
        return request(method,target,headers)
    assert status==200,(method,status)
    print(method,status,flush=True)
    return h,body
h,body=request('DESCRIBE',url,'Accept: application/sdp\r\n')
control=re.search(r'a=control:(track\S+)',body).group(1)
track=(h['content-base'].rstrip('/')+'/'+control).replace(f"rtsps://{c['ip0']}/",f"rtsps://{c['ip0']}:322/")
sets=re.search(r'sprop-parameter-sets=([^;\s]+)',body).group(1).split(',')
au=bytearray(b''.join(b'\0\0\0\1'+base64.b64decode(x) for x in sets))
h,_=request('SETUP',track,'Transport: RTP/AVP/TCP;unicast;interleaved=0-1\r\n')
session=h['session'].split(';')[0]
request('PLAY',url,'Range: npt=0.000-\r\n')
fragment=bytearray();got=False;packets=0;deadline=time.monotonic()+20
while time.monotonic()<deadline:
    pre=f.read(4);assert len(pre)==4 and pre[0]==36
    data=f.read(int.from_bytes(pre[2:],'big'))
    if pre[1]!=0:continue
    packets+=1;start=12+4*(data[0]&15)
    if data[0]&16:start+=4+4*int.from_bytes(data[start+2:start+4],'big')
    if data[0]&32:data=data[:-data[-1]]
    nal=data[start:];typ=nal[0]&31;complete=None
    if 1<=typ<=23:complete=nal
    elif typ==28:
        if nal[1]&128:fragment=bytearray([(nal[0]&224)|(nal[1]&31)])
        fragment.extend(nal[2:])
        if nal[1]&64:complete=fragment;fragment=bytearray()
    else:raise RuntimeError(f'Unexpected actual packetization {typ}')
    if complete is not None and complete[0]&31 in (5,7,8):
        au.extend(b'\0\0\0\1'+complete)
        if complete[0]&31==5:got=True
    if data[1]&128 and got:
        Path('build-win/h2d-idr.h264').write_bytes(au)
        print('H2D complete IDR:',len(au),'bytes;',packets,'RTP packets',flush=True)
        break
else:raise TimeoutError('No complete IDR')
f.close();s.close()
