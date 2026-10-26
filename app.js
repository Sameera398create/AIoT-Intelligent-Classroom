"use strict";
const el = id => document.getElementById(id);
const field = (id, value) => { el(id).textContent = value; };
const missing = v => v === null || v === undefined;
const fmt = (v, digits, unit = "") => missing(v) ? "SENSOR ERROR" : `${Number(v).toFixed(digits)}${unit}`;
let history = [];
function paintStatus(id, value, tone="") {
  field(id, value);
  el(id).classList.remove("good", "warn", "bad");
  if(tone) el(id).classList.add(tone);
}
function render(status) {
  const sample = status.latest;
  const live = status.fresh === true;
  const con = el("connection");
  con.className = `connection ${live ? "live" : (status.serial_connected ? "" : "problem")}`;
  con.querySelector("span:last-child").textContent = live ? "Live hardware data" : "Waiting / disconnected";
  field("portLabel", `Port: ${status.port || "—"}`);
  field("connectionMessage", live ? "Connected to Arduino. Displaying measurements reported by the physical sensors." : `${status.info || "Waiting for serial data."} ${status.last_data_age_sec === null ? "" : `Last report ${status.last_data_age_sec} seconds ago.`}`);
  if(!sample || !live) {
    ["temperature", "humidity", "gas", "light", "current", "vibration", "ir", "irEvents"].forEach(id => field(id, "—"));
    field("activity", "Live data unavailable"); field("energy", "Live data unavailable");
    field("lastUpdate", sample ? new Date(sample.received_at).toLocaleTimeString() : "—");
    field("lastAge", "Waiting for current report");
    field("gasStatus", "MQ analog · A0");field("lightStatus", "LDR analog · A1");field("currentStatus", "ACS712 · A2");
    return;
  }
  field("lastUpdate", new Date(sample.received_at).toLocaleTimeString());
  field("lastAge", `${status.last_data_age_sec}s old · ${status.received_count} reports received`);
  field("temperature", fmt(sample.temperature_c, 1, " °C"));
  field("humidity", fmt(sample.humidity_pct, 1, " %"));
  field("gas", `${sample.gas_raw} / 1023`);
  const gasLabels = {WARMUP:"Warming up · raw reading",ABOVE_DEMO_THRESHOLD:"Above adjustable demo threshold · not safety alarm",BELOW_DEMO_THRESHOLD:"Below adjustable demo threshold"};
  field("gasStatus", `${gasLabels[sample.gas_status] || sample.gas_status} · A0`);
  field("light", `${sample.ldr_raw} / 1023`);
  field("lightStatus", `${sample.light_status} · A1 (raw ADC)`);
  paintStatus("current", sample.load_status === "NO_LOAD" ? "NO LOAD" : sample.load_status === "ERROR" ? "SENSOR ERROR" : fmt(sample.current_a, 3, " A"), sample.load_status === "LOAD" ? "warn" : "");
  field("currentStatus", "ACS712 · A2 · thresholded RMS");
  paintStatus("vibration", sample.vibration ? "DETECTED" : "NORMAL", sample.vibration ? "warn" : "good");
  field("ir", ({OBJECT_DETECTED:"OBJECT DETECTED",OBJECT_PASSED:"OBJECT PASSED",NO_OBJECT:"NO OBJECT"})[sample.ir_status] || sample.ir_status);
  field("irEvents", String(sample.ir_events));
  field("activity", sample.evidence);
  paintStatus("energy", sample.energy_status, sample.energy_status.startsWith("POSSIBLE") ? "warn" : "");
}
const metricNames = {temperature_c:"°C",humidity_pct:"%",gas_raw:"ADC",ldr_raw:"ADC",current_a:"A"};
function chart() {
  const canvas = el("trend"), ctx = canvas.getContext("2d");
  const dpr = window.devicePixelRatio || 1;
  const width = canvas.clientWidth, height = canvas.clientHeight;
  if(!width || !height) return;
  canvas.width = Math.round(width*dpr);canvas.height=Math.round(height*dpr);
  ctx.setTransform(dpr,0,0,dpr,0,0);ctx.clearRect(0,0,width,height);
  const left=49,right=15,top=12,bottom=31,w=width-left-right,h=height-top-bottom;
  ctx.font="11px system-ui";ctx.textAlign="right";ctx.fillStyle="#8191a4";
  const key=el("chartMetric").value;
  const points=history.map((p,i)=>({i,v:p[key]})).filter(p=>Number.isFinite(p.v));
  if(points.length===0){ctx.textAlign="center";ctx.fillText("Waiting for real sensor readings",width/2,height/2);return;}
  let min=Math.min(...points.map(p=>p.v)),max=Math.max(...points.map(p=>p.v));
  if(min===max){const wiggle=Math.max(1,Math.abs(min)*.12);min-=wiggle;max+=wiggle;}
  const pad=(max-min)*.13;min-=pad;max+=pad;
  const x=i=>left+(history.length===1?w/2:i/(history.length-1)*w);
  const y=v=>top+h-(v-min)/(max-min)*h;
  ctx.strokeStyle="#e6edf4";ctx.lineWidth=1;
  for(let i=0;i<4;i++) {const gy=top+i*h/3;ctx.beginPath();ctx.moveTo(left,gy);ctx.lineTo(left+w,gy);ctx.stroke();ctx.fillText((max-i*(max-min)/3).toFixed(1),left-8,gy+4);}
  // Disconnected/missing measurements do not get interpolated as fake values.
  ctx.strokeStyle="#147db5";ctx.lineWidth=2.6;ctx.lineJoin="round";ctx.beginPath();
  let drawing=false,prevIdx=-2;
  history.forEach((row,i)=>{const val=row[key];if(!Number.isFinite(val)){drawing=false;return;}const px=x(i),py=y(val);if(!drawing||i-prevIdx!==1)ctx.moveTo(px,py);else ctx.lineTo(px,py);drawing=true;prevIdx=i;});
  ctx.stroke();ctx.fillStyle="#147db5";
  if(points.length===1){ctx.beginPath();ctx.arc(x(points[0].i),y(points[0].v),3.4,0,Math.PI*2);ctx.fill();}
  ctx.fillStyle="#8191a4";ctx.textAlign="left";
  ctx.fillText(history.length ? new Date(history[0].received_at).toLocaleTimeString() : "",left,height-7);
  ctx.textAlign="right";
  ctx.fillText(history.length ? new Date(history[history.length-1].received_at).toLocaleTimeString() : "",left+w,height-7);
  ctx.fillText(metricNames[key]||"",left+w,top-1);
}
async function poll(){
  try {
    const response=await fetch("/api/history",{cache:"no-store"});
    if(!response.ok) throw Error(`HTTP ${response.status}`);
    const status=await response.json();history=status.history||[];
    render(status);chart();
  }catch(err){render({fresh:false,serial_connected:false,info:"Dashboard server is unavailable: "+err.message,last_data_age_sec:null,latest:null});history=[];chart();}
}
el("chartMetric").addEventListener("change",chart);
window.addEventListener("resize",chart);
poll();setInterval(poll,1000);
