import shutil
import subprocess
import tempfile
import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]


class HttpCaptureComponentBehaviorTest(unittest.TestCase):
    def _dashboard_html(self, temporary_directory):
        executable = Path(temporary_directory) / "dashboard_page_dump"
        compile_result = subprocess.run(
            [
                "cc",
                "-std=c11",
                "-Wall",
                "-Wextra",
                "-Werror",
                "-I",
                str(ROOT / "components/HTTP_CAPTURE"),
                str(ROOT / "tests/host/dashboard_page_dump.c"),
                str(ROOT / "components/HTTP_CAPTURE/dashboard_page.c"),
                "-o",
                str(executable),
            ],
            cwd=ROOT,
            capture_output=True,
            text=True,
            check=False,
        )
        self.assertEqual(
            0,
            compile_result.returncode,
            msg=compile_result.stdout + compile_result.stderr,
        )
        html_result = subprocess.run(
            [str(executable)],
            cwd=ROOT,
            capture_output=True,
            text=True,
            check=False,
        )
        self.assertEqual(0, html_result.returncode, msg=html_result.stderr)
        return html_result.stdout

    def test_timer_dependency_is_explicit(self):
        cmake = (
            ROOT / "components/HTTP_CAPTURE/CMakeLists.txt"
        ).read_text(encoding="utf-8")
        self.assertIn("esp_timer", cmake)

    def test_routes_capture_ownership_and_preview_controls(self):
        with tempfile.TemporaryDirectory() as temporary_directory:
            executable = Path(temporary_directory) / "http_capture_component_test"
            compile_result = subprocess.run(
                [
                    "cc",
                    "-std=c11",
                    "-D_POSIX_C_SOURCE=200809L",
                    "-Wall",
                    "-Wextra",
                    "-Werror",
                    "-pthread",
                    "-I",
                    str(ROOT / "tests/host/include"),
                    "-I",
                    str(ROOT / "components/CAMERA/include"),
                    "-I",
                    str(ROOT / "components/WIFIAP/include"),
                    "-I",
                    str(ROOT / "components/HTTP_CAPTURE/include"),
                    "-I",
                    str(ROOT / "components/INFERENCE/include"),
                    "-I",
                    str(ROOT / "components/HEALTH/include"),
                    str(ROOT / "tests/host/http_capture_component_test.c"),
                    str(ROOT / "components/HTTP_CAPTURE/http_capture.c"),
                    str(ROOT / "components/HTTP_CAPTURE/dashboard_page.c"),
                    "-o",
                    str(executable),
                ],
                cwd=ROOT,
                capture_output=True,
                text=True,
                check=False,
            )
            self.assertEqual(
                0,
                compile_result.returncode,
                msg=compile_result.stdout + compile_result.stderr,
            )

            run_result = subprocess.run(
                [str(executable)],
                cwd=ROOT,
                capture_output=True,
                text=True,
                check=False,
            )
            self.assertEqual(
                0,
                run_result.returncode,
                msg=run_result.stdout + run_result.stderr,
            )
            self.assertIn(
                "http capture component behavior passed",
                run_result.stdout,
            )

    def test_dashboard_keeps_results_current_when_image_decode_fails(self):
        if shutil.which("node") is None:
            self.skipTest("Node.js is required for dashboard behavior testing")

        with tempfile.TemporaryDirectory() as temporary_directory:
            html = self._dashboard_html(temporary_directory)
            self.assertIn(
                ".prediction{font-size:1.25rem;font-weight:800;"
                "text-align:center;color:#155eef;margin:.25rem 0}",
                html,
            )
            script = html.split("<script>", 1)[1].split(
                "</script>", 1
            )[0]

            syntax_result = subprocess.run(
                ["node", "--check", "-"],
                cwd=ROOT,
                input=script,
                capture_output=True,
                text=True,
                check=False,
            )
            self.assertEqual(
                0,
                syntax_result.returncode,
                msg=syntax_result.stdout + syntax_result.stderr,
            )

            harness = (
                """
(async()=>{
const elements=new Map();
function element(id){
  if(!elements.has(id))elements.set(id,{
    id,textContent:'',src:'',className:'',children:[],listeners:{},
    hidden:false,disabled:false,dataset:{},attributes:{},
    classList:{toggle(){}},
    addEventListener(type,listener){this.listeners[type]=listener;},
    setAttribute(name,value){this.attributes[name]=String(value);},
    getAttribute(name){return this.attributes[name]??null;},
    append(...children){this.children.push(...children);},
    appendChild(child){this.children.push(child);},
    replaceChildren(){this.children=[];},
  });
  return elements.get(id);
}
element('inferenceSnapshot').src='blob:old';
element('prediction').textContent='old result';
global.document={
  visibilityState:'visible',listeners:{},getElementById:element,
  createElement:()=>element(Symbol()),
  addEventListener(type,listener){this.listeners[type]=listener;},
};
global.window={open(){}};
global.location={hostname:'192.168.4.1'};
global.setInterval=()=>0;
global.clearInterval=()=>{};
const revoked=[];
global.URL={
  createObjectURL:()=> 'blob:new',
  revokeObjectURL:(value)=>revoked.push(value),
};
global.Image=class{
  set src(value){this.value=value;queueMicrotask(()=>this.onerror&&this.onerror());}
};
let requestCount=0;
global.fetch=async(url)=>{
  requestCount+=1;
  if(url==='/api/inference')return{
    ok:true,
    json:async()=>({
      ready:true,sequence:1,prediction:'wet',confidence:0.9,age_ms:2,
      timing:{dsp_ms:26,classification_ms:289,anomaly_ms:0},
      scores:[{label:'wet',value:0.9}],
    }),
  };
  return{
    ok:true,status:200,headers:{get:()=> '1'},blob:async()=>({}),
  };
};
"""
                + script
                + """
await new Promise(resolve=>setTimeout(resolve,10));
if(element('inferenceSnapshot').src!=='blob:old')throw new Error('old image changed');
if(element('prediction').textContent!=='wet (0.90000)')throw new Error('result did not update');
if(element('timing').textContent!=='DSP 26 ms · classification 289 ms · anomaly 0 ms'){
  throw new Error('timing did not update');
}
if(element('scores').children.length!==1)throw new Error('scores did not update');
if(!revoked.includes('blob:new'))throw new Error('candidate URL leaked');
const requestsAfterFailure=requestCount;
await pollInference();
if(requestCount!==requestsAfterFailure+2)throw new Error('image was not retried');
})();
"""
            )
            behavior_result = subprocess.run(
                ["node", "-e", harness],
                cwd=ROOT,
                capture_output=True,
                text=True,
                check=False,
            )
            self.assertEqual(
                0,
                behavior_result.returncode,
                msg=behavior_result.stdout + behavior_result.stderr,
            )

    def test_dashboard_controls_health_and_bounds_chart_history(self):
        if shutil.which("node") is None:
            self.skipTest("Node.js is required for dashboard behavior testing")

        with tempfile.TemporaryDirectory() as temporary_directory:
            html = self._dashboard_html(temporary_directory)
            self.assertIn(
                "健康任务未开启，网页不请求健康数据，不占用监控任务栈。",
                html,
            )
            script = html.split("<script>", 1)[1].split("</script>", 1)[0]

            harness = (
                """
(async()=>{
const elements=new Map();
function element(id){
  if(!elements.has(id))elements.set(id,{
    id,textContent:'',src:'',className:'',children:[],listeners:{},
    hidden:false,disabled:false,dataset:{},attributes:{},
    classList:{toggle(){}},
    addEventListener(type,listener){this.listeners[type]=listener;},
    setAttribute(name,value){this.attributes[name]=String(value);},
    getAttribute(name){return this.attributes[name]??null;},
    append(...children){this.children.push(...children);},
    appendChild(child){this.children.push(child);},
    replaceChildren(){this.children=[];},
  });
  return elements.get(id);
}
global.document={
  visibilityState:'visible',listeners:{},getElementById:element,
  createElement:()=>element(Symbol()),
  addEventListener(type,listener){this.listeners[type]=listener;},
};
global.window={open(){}};
global.location={hostname:'192.168.4.1'};
const intervals=[];const cleared=[];
global.setInterval=(callback,delay)=>{
  const timer={id:intervals.length+1,callback,delay,name:callback.name};
  intervals.push(timer);return timer.id;
};
global.clearInterval=(id)=>cleared.push(id);
global.URL={createObjectURL:()=> 'blob:new',revokeObjectURL:()=>{}};
global.Image=class{};
let boardEnabled=false;let failHealth=false;const requests=[];
function jsonResponse(payload){return{ok:true,status:200,json:async()=>payload};}
global.fetch=async(url,options={})=>{
  requests.push({url,options});
  if(url==='/api/inference')return jsonResponse({ready:false});
  if(url==='/api/health/control'){
    const body=JSON.parse(options.body);boardEnabled=body.enabled;
    return jsonResponse(boardEnabled
      ?{enabled:true,ready:false,state:'starting'}
      :{enabled:false,ready:false,state:'off'});
  }
  if(url==='/api/health'){
    if(failHealth)throw new Error('offline');
    return jsonResponse(boardEnabled
      ?{enabled:true,ready:false,state:'starting'}
      :{enabled:false,ready:false,state:'off'});
  }
  throw new Error(`unexpected fetch ${url}`);
};
function sample(sequence){return{
  enabled:true,ready:true,sequence,state:'healthy',reason_flags:0,
  sample_age_ms:5,monitor_uptime_ms:sequence*1000,inference_age_ms:100,
  inference:{
    attempt_running:false,attempt_count:sequence,success_count:sequence,
    failure_count:0,consecutive_failure_count:0,last_error:0,
    last_attempt_started_ms:sequence*1000-220,
    last_attempt_finished_ms:sequence*1000,
    last_success_ms:sequence*1000,last_duration_ms:120+(sequence%5),
    max_duration_ms:886,stack_high_water_mark_bytes:5264,
  },
  health_stack_high_water_mark_bytes:1808,
  memory:{
    internal:{free_bytes:158000+sequence,minimum_free_bytes:137000,
      largest_free_block_bytes:82000+sequence},
    psram:{free_bytes:8261368,minimum_free_bytes:7728260,
      largest_free_block_bytes:8257536},
  },
};}
"""
                + script
                + """
await new Promise(resolve=>setTimeout(resolve,10));
if(element('healthPanelBody').hidden!==true)throw new Error('off panel expanded');
if(intervals.some(timer=>timer.name==='pollHealth'))throw new Error('off state polled');

await setHealthEnabled(true);
if(!boardEnabled)throw new Error('board was not enabled');
if(element('healthPanelBody').hidden)throw new Error('enabled panel collapsed');
if(!intervals.some(timer=>timer.name==='pollHealth'&&timer.delay===1000)){
  throw new Error('health polling did not start');
}
const control=requests.find(request=>request.url==='/api/health/control');
if(!control||control.options.method!=='POST')throw new Error('control POST missing');

for(let sequence=1;sequence<=65;sequence+=1){
  appendHealthSample(sample(sequence));
  if(sequence===1){
    if(element('latencyAge').textContent!=='0 秒前'){
      throw new Error('single-sample age is invalid');
    }
    const path=element('latencyPath').getAttribute('d');
    if(!path||path.includes('NaN')||path.includes('Infinity')){
      throw new Error('single-sample path is invalid');
    }
  }
}
if(healthHistory.length!==60)throw new Error('history is not bounded');
if(healthHistory[0].sequence!==6||healthHistory[59].sequence!==65){
  throw new Error('wrong retained sequences');
}
appendHealthSample(sample(65));
if(healthHistory.length!==60)throw new Error('duplicate sequence appended');
for(const id of ['latencyPath','memoryFreePath','memoryLargestPath']){
  const path=element(id).getAttribute('d');
  if(!path||path.includes('NaN')||path.includes('Infinity')){
    throw new Error(`invalid chart path ${id}`);
  }
}
if(element('latencyCurrent').textContent!=='当前 120 ms'){
  throw new Error('latest latency value missing');
}
if(element('latencyAge').textContent!=='59 秒前'||
   element('memoryAge').textContent!=='59 秒前'){
  throw new Error('chart time span missing');
}
for(const id of [
  'latencyTickTop','latencyTickMiddle','latencyTickBottom',
  'memoryTickTop','memoryTickMiddle','memoryTickBottom'
]){
  const label=element(id).textContent;
  if(!label||label==='—'||!Number.isFinite(Number(label))){
    throw new Error(`invalid chart tick ${id}: ${label}`);
  }
}
if(element('memoryFreeValue').textContent!=='154.4 KiB'||
   element('memoryLargestValue').textContent!=='80.1 KiB'){
  throw new Error('latest memory values missing');
}
const delayedSample=sample(66);
delayedSample.monitor_uptime_ms=126000;
appendHealthSample(delayedSample);
if(element('latencyAge').textContent!=='119 秒前'||
   element('memoryAge').textContent!=='119 秒前'){
  throw new Error('chart time span was incorrectly capped');
}

await setHealthEnabled(false);
if(boardEnabled)throw new Error('board was not disabled');
if(healthHistory.length!==0)throw new Error('history was not cleared');
if(element('healthPanelBody').hidden!==true)throw new Error('off panel stayed expanded');
for(const id of [
  'latencyCurrent','latencyTickTop','latencyTickMiddle',
  'latencyTickBottom','latencyAge','memoryFreeValue',
  'memoryLargestValue','memoryTickTop','memoryTickMiddle',
  'memoryTickBottom','memoryAge'
]){
  if(element(id).textContent!=='—'){
    throw new Error(`chart label was not cleared: ${id}`);
  }
}

failHealth=true;
await discoverHealthState();
if(element('healthSubtitle').textContent!=='连接中断 / 状态未知'){
  throw new Error('connection error was not shown');
}
})();
"""
            )
            result = subprocess.run(
                ["node", "-e", harness],
                cwd=ROOT,
                capture_output=True,
                text=True,
                check=False,
            )
            self.assertEqual(
                0,
                result.returncode,
                msg=result.stdout + result.stderr,
            )


if __name__ == "__main__":
    unittest.main()
