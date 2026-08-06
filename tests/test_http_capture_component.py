import shutil
import subprocess
import tempfile
import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]


class HttpCaptureComponentBehaviorTest(unittest.TestCase):
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

    def test_dashboard_keeps_previous_pair_when_image_decode_fails(self):
        if shutil.which("node") is None:
            self.skipTest("Node.js is required for dashboard behavior testing")

        with tempfile.TemporaryDirectory() as temporary_directory:
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
            script = html_result.stdout.split("<script>", 1)[1].split(
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
    id,textContent:'',src:'',className:'',
    addEventListener(){},appendChild(){},replaceChildren(){},
  });
  return elements.get(id);
}
element('inferenceSnapshot').src='blob:old';
element('prediction').textContent='old result';
global.document={getElementById:element,createElement:()=>element(Symbol())};
global.window={open(){}};
global.location={hostname:'192.168.4.1'};
global.setInterval=()=>0;
const revoked=[];
global.URL={
  createObjectURL:()=> 'blob:new',
  revokeObjectURL:(value)=>revoked.push(value),
};
global.Image=class{
  set src(value){this.value=value;queueMicrotask(()=>this.onerror&&this.onerror());}
};
let requestCount=0;
global.fetch=async()=>{
  requestCount+=1;
  if(requestCount===1)return{
    ok:true,
    json:async()=>({
      ready:true,sequence:1,prediction:'wet',confidence:0.9,age_ms:2,
      timing:{dsp_ms:26,classification_ms:289,anomaly_ms:0},scores:[],
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
if(element('prediction').textContent!=='old result')throw new Error('old result changed');
if(!revoked.includes('blob:new'))throw new Error('candidate URL leaked');
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


if __name__ == "__main__":
    unittest.main()
