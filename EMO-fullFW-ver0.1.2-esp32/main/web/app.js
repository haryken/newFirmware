// EMO Bridge web UI logic.
const $ = id => document.getElementById(id);
const FIELDS = ["wifi_ssid", "host_ip", "host_port", "motion_speed"];
const SECRETS = ["wifi_pass"];

// ---- config load/save ----
async function loadConfig(){
  const c = await (await fetch("/api/config")).json();
  FIELDS.forEach(k=>{ if($(k)!=null && c[k]!=null) $(k).value = c[k]; });
}
$("save").onclick = async ()=>{
  const body = {};
  FIELDS.forEach(k=>{ if($(k)) body[k]=$(k).value; });
  SECRETS.forEach(k=>{ const v=$(k).value; if(v) body[k]=v; }); // only send if typed
  if (body.host_port !== undefined) body.host_port = Number(body.host_port);
  if (body.motion_speed !== undefined) body.motion_speed = Number(body.motion_speed) || 100;
  const r = await fetch("/api/config",{method:"POST",headers:{"Content-Type":"application/json"},body:JSON.stringify(body)});
  const j = await r.json().catch(()=>({}));
  $("saved").textContent = r.ok && j.ok ? "saved ✓" : "error" + (j.error ? ": " + j.error : "");
  setTimeout(()=>$("saved").textContent="",4000);
};

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
    $("ip").textContent = s.ip || "";
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
      row("Utterance", s.session?"streaming to server":"waiting for wake word") +
      row("Utterances sent", s.sessions) +
      row("Server link", s.host_connected?"connected":"idle") +
      row("Sent / acked", `${fmt(s.fed)} / ${fmt(s.acked)}`) +
      row("Backlog", fmt(s.backlog)) +
      row("Overflow", fmt(s.overflow)) +
      row("Reconnects", s.reconnects);
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
  .then(async r=>{ const j = await r.json(); if(!j.ok) $("motst").textContent = "error: " + j.error; });
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

loadConfig(); poll(); loadControls();
