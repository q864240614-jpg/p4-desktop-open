// Execute the real page script with a held HTTP request; no network or browser service.
const fs=require('fs'), vm=require('vm'), assert=require('assert');
const html=fs.readFileSync(require('path').join(__dirname,'../todo_service/index.html'),'utf8');
const nodes=new Map();
function element(){return {hidden:true,value:'',disabled:false,classList:{toggle(){}},replaceChildren(){},append(){}};}
const context=vm.createContext({
 document:{getElementById(id){if(!nodes.has(id))nodes.set(id,element());return nodes.get(id)},createElement:element},
 fetch:()=>new Promise(()=>{}), setInterval:()=>0,
 location:{search:'',pathname:'/'},history:{replaceState(){}},URLSearchParams,Intl,Date,
});
vm.runInContext(html.match(/<script>([\s\S]*?)<\/script>/)[1],context);
assert.equal(vm.runInContext('loading',context),true);
for(const id of ['prev','next','completed','pending'])nodes.get(id).onclick();
assert.equal(vm.runInContext('page',context),0);
assert.equal(vm.runInContext('done',context),0);
vm.runInContext('loading=false;page=0;pages=1',context);
nodes.get('prev').onclick();nodes.get('next').onclick();
assert.equal(vm.runInContext('page',context),0);
vm.runInContext('loading=false;page=1;pages=3',context);
nodes.get('next').onclick();nodes.get('next').onclick();nodes.get('prev').onclick();
assert.equal(vm.runInContext('page',context),2);
console.log('PASS: in-flight navigation ignored, page boundaries held, repeated clicks issue one page change');
