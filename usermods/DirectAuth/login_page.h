#pragma once
// Self-contained login / first-run setup page served at /login by the DirectAuth usermod.
// Kept out of the cdata build pipeline on purpose so the usermod needs no core changes.
// The page asks /auth/status to decide whether to show the login form or the
// one-time setup form, then posts form-encoded credentials to /auth/login or /auth/setup.
static const char PAGE_login[] PROGMEM = R"wled(<!DOCTYPE html>
<html lang="en"><head><meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<meta name="theme-color" content="#222">
<title>WLED Login</title>
<style>
:root{--bg:#111;--card:#222;--fg:#fff;--mut:#aaa;--acc:#0f8dbf;--err:#e35}
*{box-sizing:border-box}
body{margin:0;min-height:100vh;display:flex;align-items:center;justify-content:center;background:var(--bg);color:var(--fg);font-family:system-ui,-apple-system,Segoe UI,Roboto,sans-serif}
.card{background:var(--card);border-radius:14px;padding:28px 26px;width:min(92vw,360px);box-shadow:0 8px 30px #0008}
h1{margin:0 0 4px;font-size:22px;font-weight:600}
p.sub{margin:0 0 20px;color:var(--mut);font-size:14px}
label{display:block;font-size:13px;color:var(--mut);margin:12px 0 4px}
input{width:100%;padding:11px 12px;border-radius:8px;border:1px solid #333;background:#181818;color:var(--fg);font-size:16px}
input:focus{outline:2px solid var(--acc);border-color:transparent}
button{width:100%;margin-top:20px;padding:12px;border:0;border-radius:8px;background:var(--acc);color:#fff;font-size:16px;font-weight:600;cursor:pointer}
button:disabled{opacity:.5;cursor:default}
.msg{min-height:20px;margin-top:12px;font-size:14px;color:var(--err)}
.hide{display:none}
</style></head><body>
<div class="card">
<h1 id="ttl">Sign in</h1>
<p class="sub" id="sub"></p>
<form id="f" autocomplete="on">
<label for="u">Username</label><input id="u" name="user" autocomplete="username" required maxlength="32">
<label for="p">Password</label><input id="p" name="pass" type="password" autocomplete="current-password" required maxlength="64">
<div id="c2" class="hide"><label for="p2">Confirm password</label><input id="p2" type="password" autocomplete="new-password" maxlength="64"></div>
<button id="b" type="submit">Sign in</button>
<div class="msg" id="m"></div>
</form></div>
<script>
var setup=false,to=new URLSearchParams(location.search).get('to')||'/';
if(!/^\/(?![\/\\])/.test(to))to='/';
var $=function(i){return document.getElementById(i)};
fetch('/auth/status',{cache:'no-store'}).then(function(r){return r.json()}).then(function(s){
 if(s.auth){location.replace(to);return}
 $('sub').textContent=s.name||'WLED';
 if(s.setup){setup=true;$('ttl').textContent='Create admin login';$('c2').classList.remove('hide');$('b').textContent='Create & sign in';$('u').value='admin';$('p').autocomplete='new-password';$('sub').textContent=(s.name||'WLED')+' — first-run setup'}
}).catch(function(){$('m').textContent='Cannot reach controller'});
$('f').onsubmit=function(e){
 e.preventDefault();var m=$('m'),b=$('b');m.textContent='';
 var u=$('u').value.trim(),p=$('p').value;
 if(setup){if(p.length<8){m.textContent='Password must be at least 8 characters';return}if(p!==$('p2').value){m.textContent='Passwords do not match';return}}
 b.disabled=true;
 fetch(setup?'/auth/setup':'/auth/login',{method:'POST',headers:{'Accept':'application/json'},body:new URLSearchParams({user:u,pass:p})})
 .then(function(r){return r.json().then(function(j){return{ok:r.ok,st:r.status,j:j}})})
 .then(function(r){if(r.ok){location.replace(to)}else{m.textContent=r.st==429?'Too many attempts, wait 30 s':(r.j&&r.j.error)||'Login failed';b.disabled=false}})
 .catch(function(){m.textContent='Request failed';b.disabled=false});
};
</script></body></html>)wled";
