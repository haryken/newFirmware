// EMO Bridge web UI logic.
const $ = id => document.getElementById(id);
const FIELDS = ["wifi_ssid", "host_ip", "host_port", "motion_speed", "voice_mode", "ota_url", "ws_url"];
const BOOLS = ["docked", "idle_faces", "cliff_enable", "petting_enable", "yolo_react", "explore_enable"];
const SECRETS = ["wifi_pass", "xz_token"];

// ---- config load/save ----
async function loadConfig(){
  const c = await (await fetch("/api/config")).json();
  FIELDS.forEach(k=>{ if($(k)!=null && c[k]!=null) $(k).value = c[k]; });
  BOOLS.forEach(k=>{ if($(k)!=null && c[k]!=null) $(k).checked = !!c[k]; });
}
async function postConfig(body, statusEl){
  const r = await fetch("/api/config",{method:"POST",headers:{"Content-Type":"application/json"},body:JSON.stringify(body)});
  const j = await r.json().catch(()=>({}));
  if (statusEl) {
    if (r.ok && j.ok && j.reboot) statusEl.textContent = "đã lưu — đang reboot…";
    else if (r.ok && j.ok) statusEl.textContent = "saved ✓";
    else statusEl.textContent = "error" + (j.error ? ": " + j.error : "");
    if (!(j.reboot)) setTimeout(()=> statusEl.textContent="", 4000);
  }
  return j;
}
$("save").onclick = async ()=>{
  const body = {};
  FIELDS.forEach(k=>{ if($(k)) body[k]=$(k).value; });
  BOOLS.forEach(k=>{ if($(k)) body[k]=$(k).checked; });
  SECRETS.forEach(k=>{ const v=$(k).value; if(v) body[k]=v; }); // only send if typed
  if (body.host_port !== undefined) body.host_port = Number(body.host_port);
  if (body.motion_speed !== undefined) body.motion_speed = Number(body.motion_speed) || 100;
  await postConfig(body, $("saved"));
};

// ---- Wi-Fi scan + save (SoftAP provisioning) ----
const authLabel = a => (a === 0 ? "open" : "🔒");
let wifiScanTimer = 0;
function renderWifiList(nets){
  const list = $("wifi_list");
  list.innerHTML = "";
  nets.forEach(n=>{
    const b = document.createElement("button");
    b.type = "button"; b.className = "wifi-item";
    b.innerHTML = `<span>${n.ssid || "(hidden)"}</span><span class="lock">${authLabel(n.auth)}</span><span class="rssi">${n.rssi} dBm</span>`;
    b.onclick = ()=>{
      [...list.querySelectorAll(".wifi-item")].forEach(x=>x.classList.remove("on"));
      b.classList.add("on");
      $("wifi_ssid").value = n.ssid || "";
      $("wifi_pass").focus();
    };
    list.appendChild(b);
  });
}
async function wifiScanPoll(){
  const st = $("wifi_scan_st");
  try{
    const data = await (await fetch("/api/wifi/scan")).json();
    if (data && data.scanning){
      st.textContent = "scanning…";
      wifiScanTimer = setTimeout(wifiScanPoll, 800);
      return;
    }
    const nets = Array.isArray(data) ? data : [];
    if (!nets.length){ st.textContent = "không thấy mạng — thử lại"; return; }
    st.textContent = nets.length + " mạng";
    renderWifiList(nets);
  }catch(e){ st.textContent = "scan lỗi"; }
}
function wifiScan(){
  clearTimeout(wifiScanTimer);
  $("wifi_scan_st").textContent = "scanning…";
  wifiScanPoll();
}
$("wifi_scan").onclick = wifiScan;
$("wifi_save").onclick = async ()=>{
  const ssid = ($("wifi_ssid").value || "").trim();
  const pass = $("wifi_pass").value || "";
  if (!ssid){ $("wifi_saved").textContent = "chưa chọn SSID"; return; }
  $("wifi_saved").textContent = "đang lưu…";
  await postConfig({wifi_ssid: ssid, wifi_pass: pass}, $("wifi_saved"));
};
// Auto-scan when the captive portal opens (SoftAP = port 80).
if (location.port === "80" || location.port === "") setTimeout(wifiScan, 400);

// ---- power ----
$("poweroff").onclick = async ()=>{
  if (!confirm("Turn the robot off now? It switches off completely (like holding the button) and this page stops responding.")) return;
  try{
    const r = await fetch("/api/power",{method:"POST",headers:{"Content-Type":"application/json"},body:JSON.stringify({action:"off"})});
    $("powerst").textContent = r.ok ? "powering off…" : "error";
  }catch(e){ $("powerst").textContent = "request failed"; }
};

// ---- status polling ----
const row = (k,v)=>`<b>${k}</b><span>${v}</span>`;
const fmt = n => n>1e6 ? (n/1e6).toFixed(1)+"M" : n>1e3 ? (n/1e3).toFixed(1)+"k" : n;
async function poll(){
  try{
    const s = await (await fetch("/api/status")).json();
    $("link").classList.toggle("on", s.linked);
    $("ip").textContent = s.ip ? (s.ip + ":" + (s.http_port || 8080)) : "";
    $("bar").style.width = Math.min(100, s.mic_peak/327)+"%";
    if (s.bat_valid){
      const f = $("batfill"); f.style.width = s.bat_pct+"%";
      f.className = "fill" + (s.bat_low || s.bat_pct<20 ? " low" : s.bat_pct<40 ? " mid" : "");
      $("batpct").textContent = s.bat_pct+"%";
      $("batinfo").textContent = (s.bat_mv/1000).toFixed(2)+" V · level "+s.bat_tier+"/5"
        + (s.bat_sense ? " · sense high" : "") + (s.bat_low ? " · LOW BATTERY" : "");
    } else { $("batpct").textContent = "—"; $("batinfo").textContent = "battery meter off"; }
    $("stat").innerHTML =
      row("K210 link", s.linked?"up":"down") +
      row("K210 boot", (s.boot_done?"done":"running…")+(s.version?" · fw "+s.version:"")) +
      row("Link state", ["idle (wake word armed)","listening","speaking"][s.state]||s.state) +
      row("Mic session", s.mic_open?"open":"closed") +
      row("Mic level", s.mic_peak+" / 32767") +
      row("Mic samples", fmt(s.mic_samples)) +
      row("Playback queued", s.play_queued+" smp") +
      row("Wake words heard", s.wake_events) +
      row("Voice mode", s.voice_mode || "emoa") +
      row("Utterance", s.session?"streaming to server":"waiting for wake word") +
      row("Utterances sent", s.sessions) +
      row("Server link", s.host_connected?"connected":"idle") +
      row("Xiaozhi", (s.xz_ready?"ready":"waiting") + (s.xz_activation ? " · code "+s.xz_activation : "") +
        (s.xz_speaking?" · speaking":"") + (s.xz_listening?" · listening":"")) +
      row("XZ Device-Id", s.xz_device_id || "—") +
      row("XZ Client-Id", s.xz_client_id || "—") +
      row("Opus sent/recv", `${fmt(s.xz_opus_sent||0)} / ${fmt(s.xz_opus_recv||0)}`) +
      row("Sent / acked", `${fmt(s.fed)} / ${fmt(s.acked)}`) +
      row("Backlog", fmt(s.backlog)) +
      row("Overflow", fmt(s.overflow)) +
      row("Reconnects", s.reconnects) +
      row("Behaviour", (s.beh_mode||"—") + (s.docked?" · docked":"") + (s.petting?" · petting":"")) +
      row("Cliff FOOT", s.foot_ok ? ("ok mask 0x"+(s.cliff_mask||0).toString(16)+" ["+(s.cliff||[]).join(",")+"]") : "not found") +
      row("Touch pads", s.touch_ok?"ok":"not found");
    if ($("xz_mac")) $("xz_mac").textContent = s.xz_device_id || "—";
    if ($("xz_client")) $("xz_client").textContent = s.xz_client_id || "—";
    if ($("xz_code") && s.xz_activation) $("xz_code").textContent = s.xz_activation;
    if ($("xz_st")) {
      const bits = [];
      bits.push(s.voice_mode === "xiaozhi" ? "mode=xiaozhi" : "mode="+(s.voice_mode||"emoa"));
      bits.push(s.wifi_connected ? "Wi-Fi OK" : "chưa STA / SoftAP");
      if (s.xz_activation) bits.push("cần nhập mã trên xiaozhi.me");
      else if (s.xz_ready) bits.push("đã có WS");
      else bits.push("chưa OTA");
      $("xz_st").textContent = bits.join(" · ");
    }
  }catch(e){}
  try{
    const m = await (await fetch("/api/motion")).json();
    if (!motListLoaded && m.list && m.list.length){
      $("motsel").innerHTML = m.list.map(n => "<option>" + n + "</option>").join("");
      motListLoaded = true;
    }
    let t = !m.fs_ok ? "SPIFFS not mounted: flash the storage image" :
      (m.playing ? m.name + " " + m.frame + "/" + m.frames + (m.loop ? " (loop)" : "") + " " + (m.elapsed_ms/1000).toFixed(1) + "/" + (m.expected_ms/1000).toFixed(1) + "s"
                 : (m.name ? m.name + " done in " + (m.elapsed_ms/1000).toFixed(1) + "s (file " + (m.expected_ms/1000).toFixed(1) + "s)" : m.files + " motions"));
    t += " \u00b7 x" + m.speed + (m.music_ms ? " \u00b7 song " + (m.music_ms/1000).toFixed(1) + "s" : "");
    t += " \u00b7 pose " + m.pos.join("/") + (m.powered ? "" : " \u00b7 servos off") + (m.unsupported ? " \u00b7 " + m.unsupported + " gait frames held" : "");
    $("motst").textContent = t;
  }catch(e){}
  setTimeout(poll, 1000);
}

// ---- audio test ----
$("tone").onclick  = ()=> fetch("/api/tone",{method:"POST"});
// Captures 3 s of robot mic as a WAV; the browser plays it natively in a new tab.
$("micwav").onclick = ()=> window.open("/api/mic.wav?t="+Date.now(), "_blank");

// ---- motion ----
let motListLoaded = false;
const motion = body => fetch("/api/motion",{method:"POST",headers:{"Content-Type":"application/json"},body:JSON.stringify(body)})
  .then(async r=>{ const j = await r.json(); if(!j.ok) $("motst").textContent = "error: " + j.error; return j; });
const motOpts = loop => {
  const o = {action:"play", name:$("motsel").value, loop, media:$("motmedia").checked};
  if ($("motfit").checked) o.speed = "music";
  else if (Number($("motspeed").value) > 0) o.speed = Number($("motspeed").value);
  return o;
};
$("motplay").onclick = ()=> motion(motOpts(0));
$("motloop").onclick = ()=> motion(motOpts(1));
$("motstop").onclick = ()=> motion({action:"stop"});
$("motrel").onclick  = ()=> motion({action:"release"});
$("motpose").onclick = ()=> motion({action:"pose", pos:[0,1,2,3].map(i=>Number($("p"+i).value)), ms:Number($("pms").value)});

// ---- walk test (stock gait presets, boosted amp so desk motion is obvious) ----
const walkSt = (t)=>{ $("walk_st").textContent = t; };
async function undockForWalk(){
  if ($("docked") && $("docked").checked){
    $("docked").checked = false;
    await postConfig({docked:false}, null);
  }
}
async function walkGait(kind, reverse, label){
  await undockForWalk();
  walkSt(label + "…");
  // A ~2x stock explore amp so a desk test is visible if PWM reaches the servos.
  const A = kind === "turn"
    ? (reverse ? [-20, 20, 24, 24] : [20, -20, 24, 24])
    : (reverse ? [20, 20, 20, 20] : [20, 20, 20, 20]);
  const j = await motion({
    action:"gait", kind, reverse, cycles:4, period_ms: kind==="turn"?600:400,
    A, O: kind==="turn"?[0,0,3,-3]:[0,0,2,-2],
    phase0: [0,0, reverse && kind==="walk" ? 90 : -90, reverse && kind==="walk" ? 90 : -90],
    dir: reverse && kind==="turn" ? 180 : 0
  });
  walkSt(j && j.ok ? label + " ok" : "error: " + ((j && j.error) || "?"));
}
$("walk_fwd").onclick   = ()=> walkGait("walk", false, "tiến");
$("walk_back").onclick  = ()=> walkGait("walk", true,  "lùi");
$("walk_left").onclick  = ()=> walkGait("turn", false, "trái");
$("walk_right").onclick = ()=> walkGait("turn", true,  "phải");
$("walk_stop").onclick  = async ()=>{
  await motion({action:"stop"});
  await fetch("/api/tricks",{method:"POST",headers:{"Content-Type":"application/json"},body:JSON.stringify({action:"explore_stop"})});
  walkSt("stopped");
};
$("walk_explore").onclick = async ()=>{
  await undockForWalk();
  if ($("explore_enable") && !$("explore_enable").checked){
    $("explore_enable").checked = true;
    await postConfig({explore_enable:true}, null);
  }
  walkSt("explore…");
  try{
    const r = await fetch("/api/tricks",{method:"POST",headers:{"Content-Type":"application/json"},
      body:JSON.stringify({action:"explore", seconds:20})});
    const j = await r.json();
    walkSt(j.ok ? "explore 20s" : ("error: " + (j.error||r.status)));
  }catch(e){ walkSt("explore request failed"); }
};
$("walk_wiggle").onclick = async ()=>{
  await undockForWalk();
  walkSt("SET_MODE lại…");
  await motion({action:"rmt_cmd", cmd:0xC5, data:0});
  await motion({action:"rmt_cmd", cmd:0xC3, data:0x44});
  await motion({action:"rmt_cmd", cmd:0xC4, data:0x44});
  walkSt("MCPWM wiggle…");
  await motion({action:"pose", hold:true, pos:[40,140,50,130], ms:800});
  await motion({action:"pose", hold:true, pos:[140,40,130,50], ms:800});
  await motion({action:"pose", hold:true, pos:[90,90,90,90], ms:500});
  walkSt("wiggle done");
};

// ---- robot controls: one button per entry of GET /api/controls ----
async function loadControls(){
  let list;
  try{ list = await (await fetch("/api/controls")).json(); }catch(e){ return; }
  const root = $("controls"); root.innerHTML = "";
  const groups = new Map();
  list.forEach(c=>{ if(!groups.has(c.group)) groups.set(c.group, []); groups.get(c.group).push(c); });
  for (const [g, items] of groups){
    const h = document.createElement("h3"); h.textContent = g; root.appendChild(h);
    const grid = document.createElement("div"); grid.className = "ctlgrid"; root.appendChild(grid);
    items.forEach(c=>{
      const item = document.createElement("div"); item.className = "ctlitem";
      const b = document.createElement("button"); b.className = "ghost" + (c.danger ? " danger" : ""); b.textContent = c.label;
      b.title = c.name + (c.hint ? " — " + c.hint : "") + (c.disabled ? " — SD-card/OTA command, compiled out (menuconfig EMO_ALLOW_SD_COMMANDS)" : c.danger ? " — OTA/SD-card command, asks for confirmation" : "");
      if (c.disabled) b.disabled = true;
      item.appendChild(b);
      const inputs = [];
      (c.args||[]).forEach(a=>{
        const i = document.createElement("input");
        i.value = a.def; i.placeholder = a.name; i.title = a.name; i.dataset.name = a.name; i.dataset.type = a.type;
        if (a.type === "str") i.className = "str";
        inputs.push(i); item.appendChild(i);
      });
      b.onclick = async ()=>{
        const body = {action: c.name};
        inputs.forEach(i=>{ if(i.value==="") return; body[i.dataset.name] = i.dataset.type==="num" ? Number(i.value) : i.value; });
        if (c.danger){
          if (!confirm(c.label + " is a stock OTA / file-download command: it may write the K210 SD card or start a firmware update. Send it anyway?")) return;
          body.confirm = true;
        }
        const res = $("ctlres"); res.className = ""; res.textContent = c.label + " …";
        try{
          const r = await fetch("/api/controls",{method:"POST",headers:{"Content-Type":"application/json"},body:JSON.stringify(body)});
          const j = await r.json();
          res.className = j.ok ? "ok" : "bad";
          res.textContent = c.label + ": " + (j.ok ? j.sent + (j.ack===undefined ? "" : j.ack ? "  · ACK" : "  · no ACK") : j.error);
        }catch(e){ res.className = "bad"; res.textContent = c.label + ": request failed"; }
      };
      grid.appendChild(item);
    });
  }
}

// ---- Xiaozhi pair (6-digit activation) ----
$("xz_pair").onclick = async ()=>{
  const msg = $("xz_msg");
  msg.textContent = "đang gọi OTA…";
  try{
    // Persist mode + OTA URL from the form (placeholder alone is not saved to NVS).
    const ota = (($("ota_url") && $("ota_url").value) || "").trim()
      || "https://api.tenclass.net/xiaozhi/ota/";
    if ($("ota_url")) $("ota_url").value = ota;
    if ($("voice_mode")) $("voice_mode").value = "xiaozhi";
    await postConfig({voice_mode:"xiaozhi", ota_url: ota}, null);
    const r = await fetch("/api/xiaozhi",{method:"POST",headers:{"Content-Type":"application/json"},
      body:JSON.stringify({action:"pair"})});
    const j = await r.json();
    if (j.device_id) $("xz_mac").textContent = j.device_id;
    if (j.client_id) $("xz_client").textContent = j.client_id;
    if (j.ok){
      $("xz_code").textContent = j.activation || "————";
      msg.textContent = j.activation
        ? ("mã " + j.activation + " — nhập trên xiaozhi.me")
        : (j.ready ? "đã pair / có WS, không cần mã" : "OTA OK nhưng chưa có mã");
    } else {
      msg.textContent = "lỗi: " + (j.error || r.status);
    }
  }catch(e){ msg.textContent = "request failed"; }
};
$("xz_copy").onclick = async ()=>{
  const code = ($("xz_code").textContent || "").replace(/[^0-9A-Za-z]/g,"");
  if (!code || code === "————"){ $("xz_msg").textContent = "chưa có mã"; return; }
  try{
    await navigator.clipboard.writeText(code);
    $("xz_msg").textContent = "đã copy " + code;
  }catch(e){ $("xz_msg").textContent = "copy fail — mã: " + code; }
};
$("xz_dismiss").onclick = async ()=>{
  $("xz_msg").textContent = "đang ẩn…";
  try{
    const r = await fetch("/api/xiaozhi",{method:"POST",headers:{"Content-Type":"application/json"},
      body:JSON.stringify({action:"dismiss"})});
    const j = await r.json();
    if (j.ok){
      $("xz_code").textContent = "————";
      $("xz_msg").textContent = "đã ẩn mã trên mặt";
    } else $("xz_msg").textContent = "lỗi: " + (j.error || r.status);
  }catch(e){ $("xz_msg").textContent = "request failed"; }
};

loadConfig(); poll(); loadControls();
if (location.port === "80" || location.port === "") {
  $("wifi_hint").textContent = "Điện thoại đang nối hotspot robot. Chọn Wi-Fi nhà, nhập mật khẩu, bấm Lưu & kết nối.";
}
