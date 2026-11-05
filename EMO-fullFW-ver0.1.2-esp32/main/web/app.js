// EMO Bridge web UI (wired-inspired tabs).
const $ = id => document.getElementById(id);
const FIELDS = ["wifi_ssid", "host_ip", "host_port", "motion_speed", "voice_mode", "ota_url", "ws_url"];
const BOOLS = ["docked", "idle_faces", "cliff_enable", "petting_enable", "yolo_react", "explore_enable"];
const SECRETS = ["wifi_pass", "xz_token"];
let motListLoaded = false, dailyActs = ["*"], legsOk = true;

// ---- tabs ----
document.querySelectorAll(".tabs button").forEach(btn=>{
  btn.onclick = ()=>{
    document.querySelectorAll(".tabs button").forEach(b=>b.classList.remove("active"));
    document.querySelectorAll(".container").forEach(c=>c.classList.remove("active"));
    btn.classList.add("active");
    const t = document.querySelector(btn.dataset.target);
    if (t) t.classList.add("active");
  };
});

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
  SECRETS.forEach(k=>{ const v=$(k).value; if(v) body[k]=v; });
  if (body.host_port !== undefined) body.host_port = Number(body.host_port);
  if (body.motion_speed !== undefined) body.motion_speed = Number(body.motion_speed) || 100;
  await postConfig(body, $("saved"));
};

// ---- Wi-Fi ----
const authLabel = a => (a === 0 ? "open" : "🔒");
const rssiBars = r => r > -55 ? "▂▄▆█" : r > -65 ? "▂▄▆░" : r > -75 ? "▂▄░░" : "▂░░░";
let wifiScanTimer = 0;
function renderWifiList(nets){
  const list = $("wifi_list"); list.innerHTML = "";
  nets.forEach(n=>{
    const b = document.createElement("button");
    b.type = "button"; b.className = "wifi-item";
    b.innerHTML = `<span class="bars">${rssiBars(n.rssi)}</span><span>${n.ssid || "(hidden)"}</span><span class="lock">${authLabel(n.auth)}</span><span class="rssi">${n.rssi} dBm</span>`;
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
    if (data && data.scanning){ st.textContent = "scanning…"; wifiScanTimer = setTimeout(wifiScanPoll, 800); return; }
    const nets = Array.isArray(data) ? data : [];
    if (!nets.length){ st.textContent = "không thấy mạng — thử lại"; return; }
    st.textContent = nets.length + " mạng";
    renderWifiList(nets);
  }catch(e){ st.textContent = "scan lỗi"; }
}
function wifiScan(){ clearTimeout(wifiScanTimer); $("wifi_scan_st").textContent = "scanning…"; wifiScanPoll(); }
$("wifi_scan").onclick = wifiScan;
$("wifi_save").onclick = async ()=>{
  const ssid = ($("wifi_ssid").value || "").trim();
  const pass = $("wifi_pass").value || "";
  if (!ssid){ $("wifi_saved").textContent = "chưa chọn SSID"; return; }
  $("wifi_saved").textContent = "đang lưu…";
  await postConfig({wifi_ssid: ssid, wifi_pass: pass}, $("wifi_saved"));
};
if (location.port === "80" || location.port === "") setTimeout(wifiScan, 400);

$("poweroff").onclick = async ()=>{
  if (!confirm("Tắt nguồn robot ngay?")) return;
  try{
    const r = await fetch("/api/power",{method:"POST",headers:{"Content-Type":"application/json"},body:JSON.stringify({action:"off"})});
    $("powerst").textContent = r.ok ? "powering off…" : "error";
  }catch(e){ $("powerst").textContent = "request failed"; }
};

// ---- Daily schedule UI ----
function hhmmStr(v){ const n = Number(v)||0; return String(Math.floor(n/100)).padStart(2,"0")+":"+String(n%100).padStart(2,"0"); }
function parseHhmm(s){
  if (typeof s === "number") return s;
  const m = String(s).trim().match(/^(\d{1,2}):?(\d{2})$/);
  if (m) return Number(m[1])*100 + Number(m[2]);
  return Number(s)||0;
}
function actOptions(sel){
  return dailyActs.map(a => `<option value="${a}" ${a===sel?"selected":""}>${a==="*"?"(ngẫu nhiên đặc biệt)":a}</option>`).join("");
}
function renderSlots(slots){
  const root = $("daily_slots"); root.innerHTML = "";
  (slots||[]).forEach((s,i)=>{
    const d = document.createElement("div"); d.className = "slot";
    d.innerHTML = `
      <label title="bật"><input type="checkbox" class="en" ${s.enabled!==false?"checked":""}></label>
      <input class="st" value="${hhmmStr(s.start)}" placeholder="08:00" title="bắt đầu">
      <input class="en2" value="${hhmmStr(s.end)}" placeholder="08:15" title="kết thúc">
      <select class="a">${actOptions(s.a||"Eating")}</select>
      <select class="b"><option value="">(không)</option>${actOptions(s.b||"")}</select>
      <button type="button" class="del" title="xoá">×</button>`;
    d.querySelector(".del").onclick = ()=>{ d.remove(); };
    root.appendChild(d);
  });
}
function collectSlots(){
  return [...$("daily_slots").querySelectorAll(".slot")].map(d=>({
    enabled: d.querySelector(".en").checked,
    start: parseHhmm(d.querySelector(".st").value),
    end: parseHhmm(d.querySelector(".en2").value),
    a: d.querySelector(".a").value,
    b: d.querySelector(".b").value
  }));
}
async function loadDaily(){
  try{
    const d = await (await fetch("/api/daily")).json();
    dailyActs = (d.activities && d.activities.length) ? d.activities : dailyActs;
    $("daily_en").checked = !!d.enabled;
    $("daily_tz").value = d.tz ?? 7;
    $("daily_fake").value = d.fake ?? -1;
    const clock = d.hhmm>=0 ? hhmmStr(d.hhmm) : "—";
    $("daily_info").textContent = `bây giờ ${clock} · next ${d.next||"—"} · last ${d.last||"—"}` + (d.synced?"":" · chờ SNTP");
    legsOk = d.legs_ok !== false;
    $("dock_banner").classList.toggle("on", !legsOk);
    $("dock_banner2").classList.toggle("on", !legsOk);
    renderSlots(d.slots||[]);
    renderActs();
  }catch(e){}
}
function renderActs(){
  const g = $("act_grid"); if (!g) return;
  g.innerHTML = "";
  dailyActs.filter(a=>a!=="*").forEach(a=>{
    const b = document.createElement("button");
    b.type="button"; b.textContent = a;
    b.onclick = async ()=>{
      $("act_st").textContent = "chạy "+a+"…";
      try{
        const r = await fetch("/api/daily",{method:"POST",headers:{"Content-Type":"application/json"},body:JSON.stringify({play:a})});
        $("act_st").textContent = r.ok ? ("đã gọi "+a) : "lỗi";
      }catch(e){ $("act_st").textContent = "lỗi mạng"; }
    };
    g.appendChild(b);
  });
}
$("daily_add").onclick = ()=>{
  const cur = collectSlots();
  cur.push({enabled:true,start:1200,end:1215,a:"Eating",b:""});
  renderSlots(cur);
};
$("daily_save").onclick = async ()=>{
  $("daily_st").textContent = "đang lưu…";
  const body = {
    enable: $("daily_en").checked,
    tz: Number($("daily_tz").value),
    fake: Number($("daily_fake").value),
    slots: collectSlots()
  };
  try{
    const r = await fetch("/api/daily",{method:"POST",headers:{"Content-Type":"application/json"},body:JSON.stringify(body)});
    $("daily_st").textContent = r.ok ? "đã lưu ✓" : "lỗi";
    await loadDaily();
  }catch(e){ $("daily_st").textContent = "lỗi mạng"; }
};
$("daily_reset").onclick = async ()=>{
  if (!confirm("Khôi phục lịch Daily gốc?")) return;
  await fetch("/api/daily",{method:"POST",headers:{"Content-Type":"application/json"},body:JSON.stringify({action:"reset"})});
  await loadDaily();
  $("daily_st").textContent = "đã reset";
};
$("act_random").onclick = async ()=>{
  $("act_st").textContent = "random…";
  try{
    const r = await fetch("/api/tricks",{method:"POST",headers:{"Content-Type":"application/json"},body:JSON.stringify({action:"game",game:"daily"})});
    $("act_st").textContent = r.ok ? "đã gọi daily random" : "busy/lỗi";
  }catch(e){ $("act_st").textContent = "lỗi"; }
};

// ---- status ----
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
    } else $("batpct").textContent = "—";
    const docked = !!s.docked;
    $("dock_banner").classList.toggle("on", docked || !legsOk);
    $("dock_banner2").classList.toggle("on", docked || !legsOk);
    $("stat").innerHTML =
      row("K210", (s.linked?"up":"down") + (s.boot_done?" · boot ok":" · booting") + (s.version?" · "+s.version:"")) +
      row("State", ["idle","listening","speaking"][s.state]||s.state) +
      row("Voice", s.voice_mode || "emoa") +
      row("Xiaozhi", (s.xz_ready?"ready":"waiting") + (s.xz_activation?" · code "+s.xz_activation:"") + (s.xz_speaking?" · TTS":"") + (s.xz_listening?" · listen":"")) +
      row("Behaviour", (s.beh_mode||"—") + (s.docked?" · DOCKED (chân khoá)":"") + (s.petting?" · petting":"")) +
      row("Cliff", s.foot_ok ? ("0x"+(s.cliff_mask||0).toString(16)) : "n/a") +
      row("Mic", s.mic_peak+" / 32767") +
      row("Pin", s.bat_valid ? (s.bat_pct+"% · "+(s.bat_mv/1000).toFixed(2)+"V") : "—");
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
    let t = !m.fs_ok ? "SPIFFS not mounted" :
      (m.playing ? m.name+" "+m.frame+"/"+m.frames : (m.name || (m.files+" motions")));
    t += " · pose "+(m.pos||[]).join("/") + (m.powered?"":" · servos off");
    $("motst").textContent = t;
  }catch(e){}
  setTimeout(poll, 1000);
}

$("tone").onclick  = ()=> fetch("/api/tone",{method:"POST"});
$("micwav").onclick = ()=> window.open("/api/mic.wav?t="+Date.now(), "_blank");

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
  walkSt("wiggle…");
  await motion({action:"pose", hold:true, pos:[40,140,50,130], ms:800});
  await motion({action:"pose", hold:true, pos:[140,40,130,50], ms:800});
  await motion({action:"pose", hold:true, pos:[90,90,90,90], ms:500});
  walkSt("wiggle done");
};

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
      const b = document.createElement("button"); b.textContent = c.label;
      if (c.disabled) b.disabled = true;
      item.appendChild(b);
      const inputs = [];
      (c.args||[]).forEach(a=>{
        const i = document.createElement("input");
        i.value = a.def; i.placeholder = a.name; i.dataset.name = a.name; i.dataset.type = a.type;
        if (a.type === "str") i.className = "str";
        inputs.push(i); item.appendChild(i);
      });
      b.onclick = async ()=>{
        const body = {action: c.name};
        inputs.forEach(i=>{ if(i.value==="") return; body[i.dataset.name] = i.dataset.type==="num" ? Number(i.value) : i.value; });
        if (c.danger && !confirm(c.label + " — gửi?")) return;
        if (c.danger) body.confirm = true;
        const res = $("ctlres"); res.textContent = c.label + " …";
        try{
          const r = await fetch("/api/controls",{method:"POST",headers:{"Content-Type":"application/json"},body:JSON.stringify(body)});
          const j = await r.json();
          res.textContent = c.label + ": " + (j.ok ? j.sent + (j.ack ? " · ACK" : " · no ACK") : j.error);
        }catch(e){ res.textContent = c.label + ": failed"; }
      };
      grid.appendChild(item);
    });
  }
}

$("xz_pair").onclick = async ()=>{
  const msg = $("xz_msg"); msg.textContent = "đang gọi OTA…";
  try{
    const ota = (($("ota_url") && $("ota_url").value) || "").trim() || "https://api.tenclass.net/xiaozhi/ota/";
    if ($("ota_url")) $("ota_url").value = ota;
    if ($("voice_mode")) $("voice_mode").value = "xiaozhi";
    await postConfig({voice_mode:"xiaozhi", ota_url: ota}, null);
    const r = await fetch("/api/xiaozhi",{method:"POST",headers:{"Content-Type":"application/json"},body:JSON.stringify({action:"pair"})});
    const j = await r.json();
    if (j.device_id) $("xz_mac").textContent = j.device_id;
    if (j.client_id) $("xz_client").textContent = j.client_id;
    if (j.ok){
      $("xz_code").textContent = j.activation || "————";
      msg.textContent = j.activation ? ("mã " + j.activation) : (j.ready ? "đã pair" : "OTA OK, chưa có mã");
    } else msg.textContent = "lỗi: " + (j.error || r.status);
  }catch(e){ msg.textContent = "request failed"; }
};
$("xz_copy").onclick = async ()=>{
  const code = ($("xz_code").textContent || "").replace(/[^0-9A-Za-z]/g,"");
  if (!code || code === "————"){ $("xz_msg").textContent = "chưa có mã"; return; }
  try{ await navigator.clipboard.writeText(code); $("xz_msg").textContent = "đã copy " + code; }
  catch(e){ $("xz_msg").textContent = "copy fail — " + code; }
};
$("xz_dismiss").onclick = async ()=>{
  try{
    const r = await fetch("/api/xiaozhi",{method:"POST",headers:{"Content-Type":"application/json"},body:JSON.stringify({action:"dismiss"})});
    const j = await r.json();
    if (j.ok){ $("xz_code").textContent = "————"; $("xz_msg").textContent = "đã ẩn mã"; }
    else $("xz_msg").textContent = "lỗi";
  }catch(e){ $("xz_msg").textContent = "failed"; }
};

loadConfig(); loadDaily(); poll(); loadControls();
if (location.port === "80" || location.port === "") {
  $("wifi_hint").setAttribute("data-i18n", "wifi.hint_ap");
  if (window.I18N) I18N.apply(document);
}
document.addEventListener("emo-i18n", ()=>{ /* labels refreshed by i18n.js */ });
setInterval(loadDaily, 15000);
