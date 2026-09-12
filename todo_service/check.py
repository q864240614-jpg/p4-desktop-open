"""Exercise browser login and device sync. Uses an explicit test Todo on the server."""
import json
from pathlib import Path
import urllib.request
import urllib.error
import http.cookiejar
import sys
base=sys.argv[1] if len(sys.argv)>1 else 'http://127.0.0.1:2333'
env=dict(line.split('=',1) for line in (Path(__file__).parent/'deploy.env').read_text().splitlines())
web=urllib.request.build_opener(urllib.request.HTTPCookieProcessor(http.cookiejar.CookieJar()))
def call(opener,path,body=None,device=False):
    headers={'Content-Type':'application/json'}
    if device:headers['Authorization']='Bearer '+env['TODO_DEVICE_TOKEN']
    req=urllib.request.Request(base+path,data=None if body is None else json.dumps(body).encode(),headers=headers)
    try:
        with opener.open(req,timeout=6) as r:return r.status,json.load(r)
    except urllib.error.HTTPError as e:return e.code,json.load(e)
assert call(web,'/api/todos')[0]==401
assert call(web,'/api/login',{'password':'incorrect'})[0]==401
assert call(web,'/api/login',{'password':env['TODO_PASSWORD']})[0]==200
assert call(web,'/api/todos',{'title':''})[0]==400
status,item=call(web,'/api/todos',{'title':'[同步测试] 网页发布，桌面屏完成'})
assert status==201
item_id=item['id']
device=urllib.request.build_opener()
assert any(x['id']==item_id for x in call(device,'/api/todos',device=True)[1]['items'])
assert call(device,f'/api/todos/{item_id}/complete',{},True)[0]==200
assert call(device,f'/api/todos/{item_id}/complete',{},True)[0]==200
assert any(x['id']==item_id and x['done'] for x in call(web,'/api/todos?done=1')[1]['items'])
assert call(web,'/api/logout',{})[0]==200
assert call(web,'/api/todos')[0]==401
print('PASS: login, publish, device list/complete, browser sync, idempotence, logout; test id',item_id)
