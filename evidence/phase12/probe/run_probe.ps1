# phase12 disconnect probe driver: compile + run E1/E2/E3/E4 against a local
# httplib server on 127.0.0.1:18501. Read-only experiment; product code untouched.
$ErrorActionPreference = 'Continue'
$tmp  = Join-Path $env:TEMP 'llmopt-disconnect-probe'
$inc  = 'D:\c++_myproject\llm-inference-kernel-opt\third_party\_deps\httplib-src'
Set-Location $tmp

Write-Host '=== compile probe_server ==='
g++ -std=c++20 -O2 -D_WIN32_WINNT=0x0600 "-I$inc" probe_server.cpp -o probe_server.exe -lws2_32 2>&1 | Tee-Object compile_server.log
Write-Host '=== compile probe_client ==='
g++ -std=c++20 -O2 -D_WIN32_WINNT=0x0600 "-I$inc" probe_client.cpp -o probe_client.exe -lws2_32 2>&1 | Tee-Object compile_client.log
Write-Host '=== compile probe_tcp ==='
g++ -std=c++20 -O2 -D_WIN32_WINNT=0x0600 "-I$inc" probe_tcp.cpp -o probe_tcp.exe -lws2_32 -liphlpapi 2>&1 | Tee-Object compile_tcp.log
Write-Host '=== compile-only E6 (expected to fail on req.fd / req.sock) ==='
g++ -std=c++20 -O2 -D_WIN32_WINNT=0x0600 "-I$inc" -c probe_e6.cpp -o probe_e6.o 2>&1 | Tee-Object compile_e6.log

$svr = Start-Process -FilePath "$tmp\probe_server.exe" -ArgumentList '18501' `
    -WorkingDirectory $tmp -RedirectStandardOutput "$tmp\server.log" `
    -RedirectStandardError "$tmp\server.err" -PassThru -WindowStyle Hidden
Start-Sleep -Milliseconds 1200

Write-Host '=== E1 baseline ==='
& "$tmp\probe_client.exe" e1 18501 2>&1 | Tee-Object client_e1.log
& "$tmp\probe_client.exe" ping 18501 2>&1 | Tee-Object client_ping_after_e1.log

Write-Host '=== E2 FIN disconnect ==='
& "$tmp\probe_client.exe" e2 18501 2>&1 | Tee-Object client_e2.log
Start-Sleep -Milliseconds 200
Write-Host '--- netstat snapshot after E2 ---'
netstat -ano | Select-String '18501' | Tee-Object netstat_e2.log
& "$tmp\probe_client.exe" ping 18501 2>&1 | Tee-Object client_ping_after_e2.log

Write-Host '=== E3 RST disconnect ==='
& "$tmp\probe_client.exe" e3 18501 2>&1 | Tee-Object client_e3.log
Start-Sleep -Milliseconds 200
Write-Host '--- netstat snapshot after E3 ---'
netstat -ano | Select-String '18501' | Tee-Object netstat_e3.log
& "$tmp\probe_client.exe" ping 18501 2>&1 | Tee-Object client_ping_after_e3.log

Write-Host '=== E4 chunked control ==='
& "$tmp\probe_client.exe" e4 18501 2>&1 | Tee-Object client_e4.log

Stop-Process -Id $svr.Id -Force -ErrorAction SilentlyContinue
Write-Host '=== server stderr ==='
if (Test-Path "$tmp\server.err") { Get-Content "$tmp\server.err" }
Write-Host '=== server.log ==='
Get-Content "$tmp\server.log"
